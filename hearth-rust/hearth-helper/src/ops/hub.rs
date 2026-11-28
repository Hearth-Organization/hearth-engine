//! The Hub: the project, its engine parts, other projects, and the engine on this machine.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::Duration;

use serde_json::{json, Value};

use crate::ctx::{self, p, ENGINE_BRANCH, ENGINE_REPO, VERSION};
use crate::ops::core::{last_level, op_editor_stop};
use crate::util::*;

/// The engine parts the Hub lists, in the Hub's order; gems: what each switch turns on or off.
fn part_catalogue() -> Vec<Value> {
    vec![
        json!({"name": "Atom renderer", "about": "Lighting, fog, bloom, color grading", "gems": ["Atom"], "locked": true}),
        json!({"name": "PhysX", "about": "Player movement and collisions", "gems": ["PhysX5"]}),
        json!({"name": "MiniAudio", "about": "Hum, crackles, footsteps", "gems": ["MiniAudio"]}),
        json!({"name": "Prefabs and spawning", "about": "The endless level's tile pieces", "gems": ["PrefabBuilder"], "locked": true}),
        json!({"name": "Multiplayer", "about": "Co-op squads", "gems": ["Multiplayer"], "note": "Turn on for co-op"}),
        json!({"name": "UI (LyShine)", "about": "Built-in menu system", "gems": ["LyShine"], "note": "We draw our own UI"}),
        json!({"name": "Terrain", "about": "Outdoor landscapes", "gems": ["Terrain"], "note": "Not needed yet"}),
        json!({"name": "Vegetation", "about": "Grass and trees", "gems": ["Vegetation"], "note": "Not needed yet"}),
        json!({"name": "Script Canvas", "about": "Visual scripting", "gems": ["ScriptCanvas"], "note": "Code lives in C++"}),
        json!({"name": "Cloth", "about": "Cloth simulation", "gems": ["NvCloth"], "note": "Not needed yet"}),
        json!({"name": "Profiler", "about": "Frame timings for the Profiler window", "gems": ["Profiler"], "note": "Turn on to profile"}),
    ]
}

/// The parts the Project settings window shows first (the engine's other gems are under "All parts").
fn project_parts() -> Vec<Value> {
    vec![
        json!({"name": "Renderer (Atom)", "about": "Always on", "gems": ["Atom"], "locked": true}),
        json!({"name": "Physics (PhysX)", "about": "Collisions, player body", "gems": ["PhysX5"]}),
        json!({"name": "Sound (MiniAudio)", "about": "Every sound", "gems": ["MiniAudio", "AudioSystem"]}),
        json!({"name": "Voice Studio", "about": "Lines from your Hearth server", "gems": [], "setting": "voiceStudio"}),
        json!({"name": "Python scripting", "about": "Claude and your scripts", "gems": ["EditorPythonBindings", "QtForPython"], "locked": true}),
        json!({"name": "Save games", "about": "Bases and progress", "gems": ["SaveData"]}),
        json!({"name": "Debug overlay", "about": "FPS and memory in game", "gems": ["ImGui", "DebugDraw"]}),
        json!({"name": "Animation", "about": "Monsters and hands", "gems": ["EMotionFX"]}),
        json!({"name": "Script Canvas", "about": "Visual scripting", "gems": ["ScriptCanvas"]}),
        json!({"name": "Cinematics", "about": "Cutscenes", "gems": ["Maestro"]}),
        json!({"name": "Terrain", "about": "Outdoor ground", "gems": ["Terrain"]}),
        json!({"name": "Multiplayer", "about": "Co-op over the network", "gems": ["Multiplayer"]}),
    ]
}

fn gems_of(part: &Value) -> Vec<String> {
    part.get("gems").and_then(Value::as_array).map(|a| a.iter().filter_map(|g| g.as_str().map(str::to_string)).collect()).unwrap_or_default()
}

pub fn engine_info() -> Value {
    let launch = read_json(&p().launch_file).unwrap_or(Value::Null);
    if let Some(editor) = launch.get("args").and_then(Value::as_array).and_then(|a| a.first()).and_then(Value::as_str) {
        let resolved = fs::canonicalize(editor).unwrap_or_else(|_| PathBuf::from(editor));
        if let Some(root) = resolved.ancestors().nth(5) {
            if let Some(data) = read_json(&root.join("engine.json")) {
                let version = ["display_version", "O3DEVersion", "version"].iter().find_map(|k| data.get(*k).filter(|v| truthy(Some(v))).cloned());
                return json!({"o3de_version": version, "engine_root": root.display().to_string()});
            }
        }
    }
    json!({"o3de_version": null, "engine_root": null})
}

pub fn op_parts(_args: &Value) -> Value {
    json!({"enabled": gem_names()})
}

pub fn op_set_parts(args: &Value) -> Value {
    let Some(mut data) = read_json(&p().project_json) else { return err("can't read project.json") };
    let mut gems = gem_names();
    let protected = ["NoclipProtocol", "Atom", "EditorPythonBindings", "PrefabBuilder", "PrimitiveAssets"];
    let disable = strings(args, "disable");
    let enable = strings(args, "enable");
    for name in &disable {
        if protected.contains(&name.as_str()) {
            return err(format!("{name} can't be turned off: the project needs it"));
        }
        gems.retain(|g| g != name);
    }
    for name in &enable {
        if !gems.contains(name) {
            gems.push(name.clone());
        }
    }
    let backup = p().home.join(format!("project.json.{}.bak", stamp("%Y%m%d-%H%M%S")));
    if let Err(e) = fs::copy(&p().project_json, &backup) {
        return err(format!("couldn't back up project.json: {e}"));
    }
    gems.sort_by_key(|g| g.to_lowercase());
    data["gem_names"] = json!(gems);
    if let Err(e) = write_atomic(&p().project_json, &(pretty(&data, 4) + "\n")) {
        return err(format!("couldn't write project.json: {e}"));
    }
    log(&format!("parts changed: +{enable:?} -{disable:?}"));
    json!({"ok": true, "enabled": data["gem_names"], "backup": backup.display().to_string(), "next": "run build with reconfigure true"})
}

pub fn op_hub_info(_args: &Value) -> Value {
    let data = read_json(&p().project_json).unwrap_or(json!({}));
    let gems = gem_names();
    let parts: Vec<Value> = part_catalogue()
        .into_iter()
        .map(|mut part| {
            let on = gems_of(&part).iter().all(|g| gems.contains(g));
            part["on"] = json!(on);
            part
        })
        .collect();
    let stats = load_stats();
    let launch = read_json(&p().launch_file).unwrap_or(json!({}));
    let editors = editor_processes();
    let games = game_processes();
    let mut build = ctx::get("build");
    build["last"] = stats.get("last_build").cloned().unwrap_or(Value::Null);
    let parts_on = parts.iter().filter(|x| x["on"] == true).count();
    json!({
        "agent_version": VERSION,
        "agent": "rust",
        "engine": engine_info(),
        "project": {"name": data.get("display_name").filter(|v| truthy(Some(v))).or(data.get("project_name")), "path": p().project.display().to_string(),
                    "level": last_level(), "last_opened": launch.get("seen")},
        "running": {"editor": !editors.is_empty(), "game": !games.is_empty()},
        "build": build,
        "memory": {"editor_now_mb": editors.first().and_then(|(pid, _)| rss_mb(*pid)), "game_now_mb": games.first().and_then(|(pid, _)| rss_mb(*pid)),
                   "editor_peak": stats.get("editor_memory"), "game_peak": stats.get("game_memory")},
        "parts_on": parts_on, "parts_total": parts.len(), "parts": parts,
        "pending_rebuild": stats.get("pending_rebuild").cloned().unwrap_or(json!(false)),
        "last_crash": stats.get("last_crash"),
        "free_mb": meminfo().get("MemAvailable"),
    })
}

pub fn op_set_part(args: &Value) -> Value {
    let name = text(args, "name");
    let Some(part) = part_catalogue().into_iter().find(|x| x["name"] == name) else { return err("unknown part") };
    if part.get("locked") == Some(&json!(true)) {
        return err(format!("{name} can't be turned off: the project needs it"));
    }
    let key = if flag(args, "on", false) { "enable" } else { "disable" };
    let result = op_set_parts(&json!({key: gems_of(&part)}));
    if result["ok"] == true {
        save_stats(json!({"pending_rebuild": true}));
    }
    result
}

pub fn op_new_project(args: &Value) -> Value {
    let name = text(args, "name");
    let valid = name.len() >= 2
        && name.len() <= 41
        && name.chars().next().map(|c| c.is_ascii_alphabetic()).unwrap_or(false)
        && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
    if !valid {
        return err("use letters, digits and _ (start with a letter)");
    }
    let target = p().projects_root.join(&name);
    if target.exists() {
        return err("a project with that name already exists");
    }
    let root = engine_info().get("engine_root").and_then(Value::as_str).map(PathBuf::from);
    let script = root.map(|r| r.join("scripts/o3de.sh"));
    let Some(script) = script.filter(|s| s.exists()) else { return err("can't find the engine's o3de.sh") };
    let (out, errs, code) = run(&[&script.display().to_string(), "create-project", "--project-path", &target.display().to_string()], None, Duration::from_secs(600));
    log(&format!("new project {name}: exit {code}"));
    json!({"ok": code == 0, "path": target.display().to_string(), "output": last_chars(&(out + &errs), 2000)})
}

pub fn op_switch_project(args: &Value) -> Value {
    let given = text(args, "project_path");
    let given = if let Some(rest) = given.strip_prefix("~/") { ctx::user_home().join(rest) } else { PathBuf::from(&given) };
    let target = fs::canonicalize(&given).unwrap_or(given);
    if !target.join("project.json").is_file() {
        return err("that folder isn't a project");
    }
    if Some(&target) == fs::canonicalize(&p().project).ok().as_ref() {
        return json!({"ok": true, "note": "already on this project"});
    }
    // One helper serves every game: switching only changes which game the Hub and plain requests mean. Builds
    // and jobs of the other games carry on.
    ctx::set_default(ctx::open(&target));
    log(&format!("current game is now {}", target.display()));
    crate::announce();
    json!({"ok": true, "project": target.display().to_string()})
}

pub fn op_dismiss_crash(_args: &Value) -> Value {
    let mut stats = load_stats();
    if let Some(c) = stats.get_mut("last_crash").and_then(Value::as_object_mut) {
        c.insert("dismissed".into(), json!(now()));
        let _ = fs::write(&p().stats_file, pretty(&Value::Object(stats), 2));
    }
    json!({"ok": true})
}

pub fn op_list_projects(_args: &Value) -> Value {
    let mut projects = Vec::new();
    let mut folders: Vec<PathBuf> = fs::read_dir(&p().projects_root).map(|rd| rd.flatten().map(|e| e.path()).collect()).unwrap_or_default();
    folders.sort();
    let me = fs::canonicalize(&p().project).ok();
    for folder in folders {
        let Some(data) = read_json(&folder.join("project.json")) else { continue };
        let name = data.get("display_name").filter(|v| truthy(Some(v))).or(data.get("project_name")).cloned().unwrap_or(Value::Null);
        projects.push(json!({"name": name, "path": folder.display().to_string(), "current": fs::canonicalize(&folder).ok() == me}));
    }
    json!({"projects": projects})
}

// ---------- the engine on this machine ----------

static ENGINE_LOCK: Mutex<()> = Mutex::new(());

type Step = (String, Vec<String>, PathBuf, Option<std::collections::HashMap<String, String>>);

fn engine_run(steps: Vec<Step>) {
    let _guard = ENGINE_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    if let Ok(out) = fs::OpenOptions::new().create(true).append(true).open(&p().engine_log) {
        use std::io::Write;
        for (name, cmd, cwd, env) in steps {
            ctx::set("engine", "step", json!(name));
            let mut o = &out;
            let _ = write!(o, "\n== {name}: {} ==\n", cmd.join(" "));
            log(&format!("engine {name}: {}", cmd.join(" ")));
            let code = call(&cmd, &cwd, env.as_ref(), &out);
            ctx::set("engine", "exit_code", json!(code));
            if code != 0 {
                log(&format!("engine {name} failed with code {code}"));
                ctx::set("engine", "running", json!(false));
                ctx::set("engine", "finished", json!(now()));
                return;
            }
        }
        log("engine steps finished");
    }
    ctx::set("engine", "running", json!(false));
    ctx::set("engine", "finished", json!(now()));
}

fn engine_build_step(args: &Value) -> Step {
    let mut env: std::collections::HashMap<String, String> = std::env::vars().collect();
    env.insert("GIT_TERMINAL_PROMPT".into(), "0".into());
    let requested = if truthy(args.get("jobs")) { int(args, "jobs", 2) } else { settings_jobs() };
    env.insert("HEARTH_JOBS".into(), safe_jobs(requested).to_string());
    let (high, cap) = build_caps();
    let engine = &p().engine_dir;
    let cmd = limited(vec!["bash".into(), engine.join("scripts/hearth/build-laptop.sh").display().to_string(), p().project.display().to_string()], high, cap);
    ("build".into(), cmd, engine.clone(), Some(env))
}

pub fn op_engine(args: &Value) -> Value {
    let action = args.get("action").and_then(Value::as_str).unwrap_or("status").to_string();
    let engine = p().engine_dir.clone();
    if action == "status" {
        let text_tail = tail(&p().engine_log, 40);
        let progress = text_tail.lines().rev().find(|l| l.starts_with('[') && l.split(']').next().map(|s| s.contains('/')).unwrap_or(false))
            .map(|l| l.split(']').next().unwrap_or("").trim_start_matches('[').to_string());
        let (_, _, lfs) = run(&["git", "lfs", "version"], None, Duration::from_secs(20));
        let lines: Vec<&str> = text_tail.lines().collect();
        return json!({"state": ctx::get("engine"), "progress": progress, "engine_dir": engine.display().to_string(),
                      "downloaded": engine.join("engine.json").exists(), "git_lfs": lfs == 0,
                      "disk_free_gb": disk_free_gb(&ctx::user_home()), "log_tail": lines[lines.len().saturating_sub(12)..].join("\n")});
    }
    if action == "stop" {
        let mut stopped = Vec::new();
        let dir = engine.display().to_string();
        for name in ["ninja", "cmake", "clang", "ccache", "mold"] {
            for (pid, cmd) in find_processes(name) {
                let cwd = fs::read_link(format!("/proc/{pid}/cwd")).map(|p| p.display().to_string()).unwrap_or_default();
                if (cmd.join(" ").contains(&dir) || cwd.starts_with(&dir)) && kill(pid, libc::SIGTERM) {
                    stopped.push(pid);
                }
            }
        }
        log(&format!("engine build stopped: {stopped:?}"));
        return json!({"ok": true, "stopped": stopped});
    }
    let then_build = flag(args, "then_build", false) && (action == "setup" || action == "update");
    let building_here = action == "build" || then_build;
    if building_here && setting_bool("laptop", "cloudEngineBuilds", true) && !flag(args, "local", false) {
        return json!({"ok": false, "cloud": true,
                      "error": "Hearth Settings › Build in the cloud is on: engine builds run on GitHub, not this laptop (pass local: true, or turn the setting off, to build here anyway)"});
    }
    if ENGINE_LOCK.try_lock().is_err() || ctx::engine_running_anywhere() {
        return json!({"ok": false, "error": "engine work already running", "state": ctx::get("engine")});
    }
    let mut env: std::collections::HashMap<String, String> = std::env::vars().collect();
    env.insert("GIT_TERMINAL_PROMPT".into(), "0".into());
    let mut steps: Vec<Step> = Vec::new();
    let parent = engine.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("/"));
    match action.as_str() {
        "setup" => {
            if engine.join("engine.json").exists() {
                return json!({"ok": true, "note": "already downloaded; use update"});
            }
            let _ = fs::create_dir_all(&parent);
            steps.push(("download".into(), vec!["git".into(), "clone".into(), "--depth".into(), "1".into(), "--branch".into(), ENGINE_BRANCH.into(), ENGINE_REPO.into(), engine.display().to_string()], parent, Some(env.clone())));
            steps.push(("lfs files".into(), vec!["git".into(), "lfs".into(), "pull".into()], engine.clone(), Some(env.clone())));
        }
        "update" => {
            steps.push(("update".into(), vec!["git".into(), "pull".into(), "--ff-only".into()], engine.clone(), Some(env.clone())));
            steps.push(("lfs files".into(), vec!["git".into(), "lfs".into(), "pull".into()], engine.clone(), Some(env.clone())));
        }
        "build" => {
            if !editor_processes().is_empty() {
                if !flag(args, "stop_editor", true) {
                    return err("close the editor before an engine build");
                }
                op_editor_stop(&json!({"save": true}));
            }
            wait_for_compilers(240);
            steps.push(engine_build_step(args));
        }
        _ => return err("action must be status, setup, update, build or stop"),
    }
    if then_build {
        if !editor_processes().is_empty() {
            op_editor_stop(&json!({"save": true}));
        }
        steps.push(engine_build_step(args));
    }
    let names: Vec<String> = steps.iter().map(|(n, ..)| n.clone()).collect();
    {
        let mut s = ctx::state();
        s.engine = json!({"running": true, "step": null, "started": now(), "finished": null, "exit_code": null});
    }
    ctx::spawn(move || engine_run(steps));
    json!({"ok": true, "started": names, "log": p().engine_log.display().to_string()})
}

pub fn disk_free_gb(path: &Path) -> f64 {
    let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
    let c = std::ffi::CString::new(path.display().to_string()).unwrap_or_default();
    if unsafe { libc::statvfs(c.as_ptr(), &mut st) } == 0 {
        (st.f_bavail as f64 * st.f_frsize as f64 / (1u64 << 30) as f64 * 10.0).round() / 10.0
    } else {
        0.0
    }
}

// ---------- Project settings: engine parts ----------

fn project_settings() -> Value {
    read_json(&p().project_settings_file).and_then(|v| v.get("Hearth").and_then(|h| h.get("Project")).cloned()).unwrap_or(json!({}))
}

/// Every gem the engine and the project offer: {name: summary}.
fn engine_gems() -> Vec<(String, String)> {
    let mut roots: Vec<String> = Vec::new();
    for r in [engine_info().get("engine_root").and_then(Value::as_str).map(str::to_string), Some("/opt/hearth-engine".into()), Some(p().engine_dir.display().to_string())].into_iter().flatten() {
        if Path::new(&r).is_dir() && !roots.contains(&r) {
            roots.push(r);
        }
    }
    let mut search: Vec<String> = roots.into_iter().take(1).collect();
    search.push(p().project.display().to_string());
    let mut found: Vec<(String, String)> = Vec::new();
    for root in search {
        let mut files = glob(&format!("{root}/Gems/*/gem.json"));
        files.extend(glob(&format!("{root}/Gems/*/*/gem.json")));
        files.extend(glob(&format!("{root}/Gem/gem.json")));
        for f in files {
            let Some(data) = read_json(&f) else { continue };
            let Some(name) = data.get("gem_name").and_then(Value::as_str) else { continue };
            if !found.iter().any(|(n, _)| n == name) {
                let about = ["summary", "display_name"].iter().find_map(|k| data.get(*k).and_then(Value::as_str).filter(|s| !s.is_empty())).unwrap_or("");
                found.push((name.to_string(), about.to_string()));
            }
        }
    }
    found.sort_by_key(|(n, _)| n.to_lowercase());
    found
}

pub fn op_project_parts(_args: &Value) -> Value {
    let gems = gem_names();
    let stats = load_stats();
    let built_list: Vec<String> = stats.get("built_gems").and_then(Value::as_array).map(|a| a.iter().filter_map(|g| g.as_str().map(str::to_string)).collect()).unwrap_or_default();
    let built = if built_list.is_empty() && !truthy(stats.get("pending_rebuild")) {
        let mut sorted = gems.clone();
        sorted.sort();
        save_stats(json!({"built_gems": sorted})); // nothing changed yet: what's on now is what was built
        gems.clone()
    } else {
        built_list
    };
    let settings = project_settings();
    let mut parts = Vec::new();
    for part in project_parts() {
        let (on, changed) = if let Some(setting) = part.get("setting").and_then(Value::as_str) {
            (settings.get(setting).map(|v| truthy(Some(v))).unwrap_or(true), false)
        } else {
            let g = gems_of(&part);
            let on = g.iter().all(|x| gems.contains(x));
            (on, on != g.iter().all(|x| built.contains(x)))
        };
        parts.push(json!({"name": part["name"], "about": part["about"], "on": on, "locked": part.get("locked") == Some(&json!(true)), "changed": changed}));
    }
    let available = engine_gems();
    let curated: Vec<String> = project_parts().iter().flat_map(gems_of).collect();
    let everything: Vec<Value> = available
        .iter()
        .map(|(name, about)| json!({"gem": name, "about": about, "on": gems.contains(name), "changed": gems.contains(name) != built.contains(name)}))
        .collect();
    let changed = parts.iter().filter(|x| x["changed"] == true).count()
        + everything.iter().filter(|g| g["changed"] == true && !curated.iter().any(|c| g["gem"] == c.as_str())).count();
    json!({"parts": parts, "all": everything, "on": gems.len(), "total": available.len().max(gems.len()), "changed": changed,
           "pending_rebuild": stats.get("pending_rebuild").cloned().unwrap_or(json!(false))})
}

pub fn op_set_project_part(args: &Value) -> Value {
    let on = flag(args, "on", false);
    let name = text(args, "name");
    let part = project_parts().into_iter().find(|x| x["name"] == name);
    if let Some(part) = &part {
        if part.get("locked") == Some(&json!(true)) {
            return err(format!("{name} can't be turned off: the project needs it"));
        }
        if let Some(setting) = part.get("setting").and_then(Value::as_str) {
            let file = &p().project_settings_file;
            let mut data = read_json(file).filter(Value::is_object).unwrap_or(json!({}));
            if !data.get("Hearth").map(Value::is_object).unwrap_or(false) {
                data["Hearth"] = json!({});
            }
            if !data["Hearth"].get("Project").map(Value::is_object).unwrap_or(false) {
                data["Hearth"]["Project"] = json!({});
            }
            data["Hearth"]["Project"][setting] = json!(on);
            if let Some(dir) = file.parent() {
                let _ = fs::create_dir_all(dir);
            }
            return match write_atomic(file, &(pretty(&data, 4) + "\n")) {
                Ok(()) => json!({"ok": true}),
                Err(e) => err(format!("couldn't save the project settings: {e}")),
            };
        }
    }
    let gems = match &part {
        Some(x) => gems_of(x),
        None => vec![text(args, "gem")],
    };
    if gems.is_empty() || gems[0].is_empty() {
        return err("unknown part");
    }
    let result = op_set_parts(&json!({if on { "enable" } else { "disable" }: gems}));
    if result["ok"] == true {
        save_stats(json!({"pending_rebuild": true}));
    }
    result
}
