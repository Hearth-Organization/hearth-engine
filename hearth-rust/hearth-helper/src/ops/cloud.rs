//! Building and packaging the game on GitHub (or packaging on this computer).

use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde_json::{json, Value};

use crate::ctx::{self, p};
use crate::ops::hub::engine_info;
use crate::ops::levelgen::{op_levelgen_get, op_levelgen_tests};
use crate::util::*;

const CLOUD_WORKFLOW: &str = "hearth-build.yml";
const PACKAGE_WORKFLOW: &str = "hearth-package.yml";

fn git(args: &[&str], timeout: u64) -> (String, String, i32) {
    let mut cmd = vec!["git"];
    cmd.extend_from_slice(args);
    run(&cmd, Some(&p().project), Duration::from_secs(timeout))
}

fn gh(args: &[&str], timeout: u64) -> (String, String, i32) {
    let mut cmd = vec!["gh"];
    cmd.extend_from_slice(args);
    run(&cmd, Some(&p().project), Duration::from_secs(timeout))
}

fn branch() -> String {
    let b = git(&["rev-parse", "--abbrev-ref", "HEAD"], 60).0.trim().to_string();
    if b.is_empty() { "main".into() } else { b }
}

/// Waits for the newest run of a workflow to finish; its id.
fn follow_run(workflow: &str, branch: Option<&str>, which: &str, step: &str) -> Result<i64, String> {
    std::thread::sleep(Duration::from_secs(8));
    let mut list = vec!["run", "list", "--workflow", workflow];
    if let Some(b) = branch {
        list.extend(["--branch", b]);
    }
    list.extend(["-L", "1", "--json", "databaseId"]);
    let (out, _, _) = gh(&list, 60);
    let run_id = serde_json::from_str::<Value>(if out.trim().is_empty() { "[]" } else { &out })
        .ok()
        .and_then(|v| v.get(0).and_then(|r| r.get("databaseId")).and_then(Value::as_i64))
        .ok_or("GitHub didn't list the run")?;
    ctx::set(which, "run_id", json!(run_id));
    ctx::set(which, "step", json!(step));
    loop {
        let id = run_id.to_string();
        let (out, _, _) = gh(&["run", "view", &id, "--json", "status,conclusion"], 60);
        let info: Value = serde_json::from_str(if out.trim().is_empty() { "{}" } else { &out }).unwrap_or(json!({}));
        if info["status"] == "completed" {
            if info["conclusion"] != "success" {
                return Err(format!("{} ended: {}", if which == "cloud" { "the GitHub build" } else { "packaging on GitHub" }, info["conclusion"].as_str().unwrap_or("?")));
            }
            return Ok(run_id);
        }
        std::thread::sleep(Duration::from_secs(20));
    }
}

/// Build the game on GitHub and bring the result back into build/linux. `commit` lets it commit and push
/// project.json and Registry first (the owner pressed the button and agreed).
pub fn op_cloud_build(args: &Value) -> Value {
    if ctx::running("cloud") {
        return json!({"ok": true, "state": ctx::get("cloud")});
    }
    if which("gh").is_none() {
        return err("the GitHub app (gh) isn't installed on this computer");
    }
    if gh(&["auth", "status"], 60).2 != 0 {
        return err("gh isn't signed in: run gh auth login once");
    }
    if !p().project.join(".github/workflows").join(CLOUD_WORKFLOW).exists() {
        return err(format!("this project has no .github/workflows/{CLOUD_WORKFLOW}"));
    }
    let changed = git(&["status", "--porcelain", "--", "project.json", "Registry"], 60).0.trim().to_string();
    if !changed.is_empty() && !flag(args, "commit", false) {
        return json!({"ok": false, "needs_commit": true, "files": changed.lines().collect::<Vec<_>>(),
                      "error": "GitHub builds what's pushed: the parts change has to be committed and pushed first"});
    }
    let b = branch();
    if !changed.is_empty() {
        git(&["add", "--", "project.json", "Registry"], 60);
        let (o, e, code) = git(&["commit", "-m", "Engine parts changed in Project settings"], 60);
        if code != 0 {
            return err(format!("couldn't commit: {}", last_chars(if e.is_empty() { &o } else { &e }, 400)));
        }
    }
    let (_, e, code) = git(&["push", "origin", &b], 300);
    if code != 0 {
        return err(format!("couldn't push: {}", last_chars(&e, 400)));
    }
    {
        let mut s = ctx::state();
        s.cloud = json!({"running": true, "step": "starting on GitHub", "run_id": null, "error": null, "finished": null});
    }
    ctx::spawn(move || {
        let result = (|| -> Result<(), String> {
            let (_, e, code) = gh(&["workflow", "run", CLOUD_WORKFLOW, "--ref", &b], 120);
            if code != 0 {
                return Err(last_chars(&e, 400));
            }
            let run_id = follow_run(CLOUD_WORKFLOW, Some(&b), "cloud", "building on GitHub")?;
            ctx::set("cloud", "step", json!("bringing the build home"));
            let target = p().home.join("cloud-build");
            let _ = fs::remove_dir_all(&target);
            let id = run_id.to_string();
            let t = target.display().to_string();
            let (_, e, code) = gh(&["run", "download", &id, "-n", "game-build", "-D", &t], 1800);
            if code != 0 {
                return Err(last_chars(&e, 400));
            }
            let out = p().project.join("build/linux");
            let _ = fs::create_dir_all(&out);
            let archive = target.join("game-build.tar.zst").display().to_string();
            let o = out.display().to_string();
            let (_, e, code) = run(&["tar", "--zstd", "-xf", &archive, "-C", &o], None, Duration::from_secs(600));
            if code != 0 {
                return Err(last_chars(&e, 400));
            }
            save_stats(json!({"pending_rebuild": false, "built_gems": gem_names(),
                              "last_build": {"ok": true, "finished": now(), "steps": "GitHub", "seconds": null, "errors": [], "log": format!("GitHub run {run_id}")}}));
            ctx::set("cloud", "step", json!("done"));
            log(&format!("cloud build {run_id} brought home"));
            Ok(())
        })();
        if let Err(e) = result {
            ctx::set("cloud", "error", json!(e));
            log(&format!("cloud build failed: {e}"));
        }
        ctx::set("cloud", "running", json!(false));
        ctx::set("cloud", "finished", json!(now()));
    });
    json!({"ok": true, "started": true})
}

pub fn op_cloud_status(_args: &Value) -> Value {
    ctx::get("cloud")
}

// ---------- Package the game ----------

fn asset_db() -> Option<rusqlite::Connection> {
    let db = p().project.join("Cache/assetdb.sqlite");
    if !db.exists() {
        return None;
    }
    let conn = rusqlite::Connection::open_with_flags(&db, rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY).ok()?;
    let _ = conn.busy_timeout(Duration::from_secs(5));
    Some(conn)
}

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    if let Ok(rd) = fs::read_dir(dir) {
        for e in rd.flatten() {
            let path = e.path();
            match e.file_type() {
                Ok(t) if t.is_dir() => walk(&path, out),
                Ok(t) if t.is_file() => out.push(path),
                _ => {}
            }
        }
    }
}

pub fn op_package_checks(args: &Value) -> Value {
    let mut levels = strings(args, "levels");
    if levels.is_empty() {
        levels = fs::read_dir(p().project.join("Levels"))
            .map(|rd| rd.flatten().filter(|e| e.path().is_dir()).map(|e| e.file_name().to_string_lossy().into_owned()).collect())
            .unwrap_or_default();
        levels.sort();
    }
    let db = asset_db();
    let mut checks = Vec::new();
    // Status 2/3 = failed, 4 = completed (AzToolsFramework::AssetSystem::JobStatus).
    let mut bad = Vec::new();
    for level in &levels {
        let statuses: Vec<i64> = db
            .as_ref()
            .and_then(|c| {
                let mut st = c.prepare("SELECT Jobs.Status FROM Jobs JOIN Sources ON Jobs.SourcePK = Sources.SourceID WHERE lower(Sources.SourceName) = lower(?1)").ok()?;
                let rows = st.query_map([format!("Levels/{level}/{level}.prefab")], |r| r.get::<_, i64>(0)).ok()?;
                Some(rows.flatten().collect())
            })
            .unwrap_or_default();
        if statuses.is_empty() || statuses.iter().any(|s| *s == 2 || *s == 3) {
            bad.push(level.clone());
        }
    }
    checks.push(json!({"ok": bad.is_empty(), "text": if bad.is_empty() { "All levels open without errors".to_string() } else { format!("These levels have problems: {}", bad.join(", ")) }}));
    if p().project.join("Levels/Level0").is_dir() && levels.iter().any(|l| l == "Level0") {
        let current = op_levelgen_get(&json!({})).get("values").cloned().unwrap_or(json!({}));
        let tests = op_levelgen_tests(&json!({"values": current, "seeds": 50}));
        let passed = tests["ok"] == true;
        let why = tests.get("error").or(tests.get("output")).map(|v| v.as_str().map(str::to_string).unwrap_or_else(|| v.to_string())).unwrap_or_default();
        checks.push(json!({"ok": passed, "text": if passed { "Level 0 layout tests pass (50 seeds)".to_string() } else { format!("Level 0 layout tests fail: {}", why.chars().take(200).collect::<String>()) }}));
    }
    let failed: Vec<String> = db
        .as_ref()
        .and_then(|c| {
            let mut st = c.prepare("SELECT DISTINCT Sources.SourceName FROM Jobs JOIN Sources ON Jobs.SourcePK = Sources.SourceID WHERE Jobs.Status IN (2, 3)").ok()?;
            let rows = st.query_map([], |r| r.get::<_, String>(0)).ok()?;
            Some(rows.flatten().collect())
        })
        .unwrap_or_default();
    let mut level_text = String::new();
    for level in &levels {
        if let Ok(t) = fs::read_to_string(p().project.join(format!("Levels/{level}/{level}.prefab"))) {
            level_text.push_str(&t.to_lowercase());
        }
    }
    let mut prefabs = Vec::new();
    walk(&p().project.join("Prefabs"), &mut prefabs);
    for f in prefabs.iter().filter(|f| f.extension().map(|e| e == "prefab").unwrap_or(false)) {
        if let Ok(t) = fs::read_to_string(f) {
            level_text.push_str(&t.to_lowercase());
        }
    }
    let stem = |n: &str| Path::new(n).file_stem().map(|s| s.to_string_lossy().to_lowercase()).unwrap_or_default();
    let used: Vec<&String> = failed.iter().filter(|n| level_text.contains(&stem(n))).collect();
    if !failed.is_empty() {
        let mut t = format!("{} file{} failed to process", failed.len(), if failed.len() != 1 { "s" } else { "" });
        if used.is_empty() {
            t.push_str(" (not used in the game)");
        } else {
            let names: Vec<String> = used.iter().take(3).map(|u| Path::new(u.as_str()).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()).collect();
            t.push_str(&format!(" ({} used in the game: {})", used.len(), names.join(", ")));
        }
        checks.push(json!({"ok": used.is_empty(), "warn": used.is_empty(), "text": t, "files": failed.iter().take(20).collect::<Vec<_>>()}));
    }
    // Estimate: what the levels' assets take in the cache, about two thirds of that once bundled.
    let mut cache = Vec::new();
    walk(&p().project.join("Cache/linux"), &mut cache);
    let size: u64 = cache.iter().filter_map(|f| fs::metadata(f).ok()).map(|m| m.len()).sum();
    let stats = load_stats();
    let minutes = stats.get("package_minutes").and_then(Value::as_f64).filter(|m| *m > 0.0).unwrap_or(12.0);
    json!({"checks": checks, "download_bytes": (size as f64 * 0.66) as u64, "github_minutes": minutes,
           "github_cost": (minutes * 0.082 * 100.0).round() / 100.0, "last_package": stats.get("last_package")})
}

/// Package the game. args: target (hearthos | steam | test), version (the owner's), levels, leave_out_editor, cloud.
pub fn op_package(args: &Value) -> Value {
    if ctx::running("package") {
        return json!({"ok": true, "state": ctx::get("package")});
    }
    let target = text(args, "target");
    let version = text(args, "version").trim().to_string();
    let levels = strings(args, "levels");
    if !["hearthos", "steam", "test"].contains(&target.as_str()) {
        return err("target must be hearthos, steam or test");
    }
    if version.is_empty() {
        return err("a version is needed: the owner picks it");
    }
    if levels.is_empty() {
        return err("pick at least one level");
    }
    let leave_out = flag(args, "leave_out_editor", true);
    let cloud = flag(args, "cloud", true);
    let Some(data) = read_json(&p().project_json) else { return err("can't read project.json") };
    let name = data.get("project_name").and_then(Value::as_str).unwrap_or("").to_string();
    let display = data.get("display_name").and_then(Value::as_str).filter(|s| !s.is_empty()).unwrap_or(&name).to_string();
    let icon = p().project.join(data.get("icon_path").and_then(Value::as_str).unwrap_or("preview.png"));
    let out = p().project.join("build/packages");
    {
        let mut s = ctx::state();
        s.package = json!({"running": true, "step": "starting", "error": null, "finished": null, "result": null});
    }
    ctx::spawn(move || {
        let started = now();
        let result = (|| -> Result<(Vec<String>, String), String> {
            if cloud {
                if which("gh").is_none() {
                    return Err("the GitHub app (gh) isn't installed on this computer".into());
                }
                let b = branch();
                let t = format!("target={target}");
                let v = format!("version={version}");
                let l = format!("levels={}", levels.join(","));
                let lo = format!("leave_out_editor={}", if leave_out { "true" } else { "false" });
                let (_, e, code) = gh(&["workflow", "run", PACKAGE_WORKFLOW, "--ref", &b, "-f", &t, "-f", &v, "-f", &l, "-f", &lo], 120);
                if code != 0 {
                    return Err(last_chars(&e, 400));
                }
                let run_id = follow_run(PACKAGE_WORKFLOW, None, "package", "packaging on GitHub")?;
                ctx::set("package", "step", json!("bringing the package home"));
                let dir = p().home.join("package-download");
                let _ = fs::remove_dir_all(&dir);
                let id = run_id.to_string();
                let d = dir.display().to_string();
                let (_, e, code) = gh(&["run", "download", &id, "-n", "package", "-D", &d], 3600);
                if code != 0 {
                    return Err(last_chars(&e, 400));
                }
                let _ = fs::create_dir_all(p().project.join("build"));
                let archive = dir.join("package.tar.zst").display().to_string();
                let build = p().project.join("build").display().to_string();
                let (_, e, code) = run(&["tar", "--zstd", "-xf", &archive, "-C", &build], None, Duration::from_secs(3600));
                if code != 0 {
                    return Err(format!("couldn't unpack the package: {}", last_chars(&e, 400)));
                }
                let files: Vec<String> = glob(&format!("{}/*{version}*", out.display())).iter().map(|f| f.display().to_string()).collect();
                let where_ = format!("GitHub run {run_id}{}", if target == "hearthos" { format!(" · draft release v{version}") } else { String::new() });
                save_stats(json!({"package_minutes": ((now() - started) / 60.0).round()}));
                Ok((files, where_))
            } else {
                let engine = engine_info().get("engine_root").and_then(Value::as_str).unwrap_or("/opt/hearth-engine").to_string();
                let o3de = Path::new(&engine).join("scripts/o3de.sh");
                let export_root = p().project.join("build/export");
                let _ = fs::remove_dir_all(&export_root);
                let mut command = vec![
                    o3de.display().to_string(), "export-project".into(), "-es".into(),
                    Path::new(&engine).join("scripts/o3de/ExportScripts/export_source_built_project.py").display().to_string(),
                    "-pp".into(), p().project.display().to_string(), "-out".into(), export_root.display().to_string(),
                    "-cfg".into(), "release".into(), "--fail-on-asset-errors".into(),
                ];
                for level in &levels {
                    command.push("--level-name".into());
                    command.push(level.clone());
                }
                ctx::set("package", "step", json!("packaging on this laptop"));
                let (high, cap) = build_caps();
                let log_path = p().home.join("package.log");
                let code = match fs::File::create(&log_path) {
                    Ok(f) => call(&limited(command, high, cap), &p().project, None, &f),
                    Err(e) => return Err(e.to_string()),
                };
                if code != 0 {
                    return Err(format!("packaging failed: {}", tail(&log_path, 20)));
                }
                let exported = fs::read_dir(&export_root).ok().and_then(|rd| rd.flatten().map(|e| e.path()).find(|p| p.is_dir())).unwrap_or(export_root.clone());
                ctx::set("package", "step", json!("finishing"));
                let mut finish = vec![
                    "python3".to_string(), p().project.join("Tools/HearthPackage/finish_package.py").display().to_string(),
                    "--export".into(), exported.display().to_string(), "--target".into(), target.clone(), "--name".into(), name.clone(),
                    "--display".into(), display.clone(), "--version".into(), version.clone(), "--out".into(), out.display().to_string(),
                    "--icon".into(), icon.display().to_string(),
                ];
                if leave_out {
                    finish.push("--leave-out-editor".into());
                }
                let refs: Vec<&str> = finish.iter().map(String::as_str).collect();
                let (so, se, code) = run(&refs, None, Duration::from_secs(1800));
                if code != 0 {
                    return Err(last_chars(if se.is_empty() { &so } else { &se }, 600));
                }
                Ok((so.lines().filter_map(|l| l.strip_prefix("PACKAGE ")).map(str::to_string).collect(), "this laptop".into()))
            }
        })();
        match result {
            Ok((files, where_)) => {
                ctx::set("package", "step", json!("done"));
                ctx::set("package", "result", json!({"files": files, "where": where_}));
                save_stats(json!({"last_package": {"version": version, "target": target, "when": now(), "where": where_, "files": files}}));
                log(&format!("packaged {version} for {target} ({where_})"));
            }
            Err(e) => {
                ctx::set("package", "error", json!(e));
                log(&format!("packaging failed: {e}"));
            }
        }
        ctx::set("package", "running", json!(false));
        ctx::set("package", "finished", json!(now()));
    });
    json!({"ok": true, "started": true})
}

pub fn op_package_status(_args: &Value) -> Value {
    ctx::get("package")
}
