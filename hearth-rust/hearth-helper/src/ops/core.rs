//! The editor, the game, builds, logs and crash reports.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::Duration;

use serde_json::{json, Value};

use crate::ctx::{self, p, VERSION};
use crate::util::*;

// ---------- the editor's bridge (Hearth Link, over its file inbox like the Python helper) ----------

pub fn bridge_alive(max_age: f64) -> bool {
    mtime(&p().bridge.join("heartbeat.json")).map(|t| now() - t < max_age).unwrap_or(false)
}

/// Runs Python inside the editor through the bridge's file inbox: {"ok", "output", "error"}, or None.
pub fn bridge_python(code: &str, timeout: u64) -> Option<Value> {
    let bridge = hearth_link::Bridge::connect(&p().project)
        .ok()?
        .transport(hearth_link::Transport::Files)
        .timeout(Duration::from_secs(timeout));
    let mut fields = serde_json::Map::new();
    fields.insert("code".into(), json!(code));
    let reply = bridge.request("python", fields).ok()?;
    Some(json!({"ok": reply.ok, "output": reply.output, "error": reply.error}))
}

// ---------- how the editor is started ----------

/// When the editor runs, keep exactly how it was started, so it can be started the same way again.
pub fn remember_editor_launch() {
    if let Some((pid, args)) = editor_processes().into_iter().next() {
        let Ok(raw) = fs::read(format!("/proc/{pid}/environ")) else { return };
        let keep = [
            "DISPLAY", "XAUTHORITY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "QT_QPA_PLATFORM", "XDG_SESSION_TYPE",
            "DBUS_SESSION_BUS_ADDRESS", "HOME", "PATH", "LD_LIBRARY_PATH", "LANG",
        ];
        let mut env = serde_json::Map::new();
        for item in raw.split(|b| *b == 0) {
            let s = String::from_utf8_lossy(item);
            if let Some((k, v)) = s.split_once('=') {
                if keep.contains(&k) {
                    env.insert(k.to_string(), json!(v));
                }
            }
        }
        let cwd = fs::read_link(format!("/proc/{pid}/cwd")).map(|p| p.display().to_string()).unwrap_or_default();
        let data = json!({"args": args, "cwd": cwd, "env": env, "seen": now()});
        let _ = fs::write(&p().launch_file, pretty(&data, 2));
    }
}

/// The Editor of the engine this project uses, so a remembered launch of another engine is never reused.
pub fn project_engine_editor() -> Option<String> {
    let mut roots: Vec<String> = Vec::new();
    if let Some(v) = read_json(&p().project.join("user/project.json")) {
        roots.push(v.get("engine_path").and_then(Value::as_str).unwrap_or("").to_string());
    }
    if let Some(name) = read_json(&p().project_json).and_then(|v| v.get("engine").and_then(Value::as_str).map(str::to_string)) {
        if let Some(manifest) = read_json(&ctx::user_home().join(".o3de/o3de_manifest.json")) {
            for root in manifest.get("engines").and_then(Value::as_array).cloned().unwrap_or_default() {
                let Some(root) = root.as_str() else { continue };
                if read_json(&Path::new(root).join("engine.json"))
                    .and_then(|e| e.get("engine_name").and_then(Value::as_str).map(|n| n == name))
                    .unwrap_or(false)
                {
                    roots.push(root.to_string());
                }
            }
        }
    }
    roots
        .into_iter()
        .filter(|r| !r.is_empty())
        .map(|r| Path::new(&r).join("bin/Linux/profile/Default/Editor"))
        .find(|e| e.exists())
        .map(|e| e.display().to_string())
}

pub fn default_editor_launch() -> Option<Value> {
    let mut candidates: Vec<PathBuf> = ["/opt/hearth-engine/bin/Linux/profile/Default/Editor", "/var/opt/hearth-engine/bin/Linux/profile/Default/Editor"]
        .iter()
        .map(PathBuf::from)
        .filter(|c| c.exists())
        .take(1)
        .collect();
    let mut more = glob("/var/opt/O3DE/*/bin/Linux/profile/Default/Editor");
    more.reverse();
    candidates.extend(more);
    let mut mine = glob("~/O3DE/Engines/*/bin/Linux/profile/Default/Editor");
    mine.reverse();
    candidates.extend(mine);
    let first = candidates.first()?;
    Some(json!({"args": [first.display().to_string(), format!("--project-path={}", p().project.display())],
                "cwd": p().project.display().to_string(), "env": {}}))
}

fn launch_file() -> Value {
    read_json(&p().launch_file).unwrap_or(Value::Null)
}

fn args_of(launch: &Value) -> Vec<String> {
    launch.get("args").and_then(Value::as_array).map(|a| a.iter().filter_map(|x| x.as_str().map(str::to_string)).collect()).unwrap_or_default()
}

fn realpath(s: &str) -> PathBuf {
    fs::canonicalize(s).unwrap_or_else(|_| PathBuf::from(s))
}

// ---------- operations ----------

pub fn op_status(_args: &Value) -> Value {
    let m = meminfo();
    let build = ctx::get("build");
    json!({
        "agent_version": VERSION,
        "agent": "rust",
        "editor": editor_processes().iter().map(|(pid, _)| json!({"pid": pid, "memory_mb": rss_mb(*pid)})).collect::<Vec<_>>(),
        "editor_bridge_alive": bridge_alive(10.0),
        "game": game_processes().iter().map(|(pid, _)| json!({"pid": pid, "memory_mb": rss_mb(*pid)})).collect::<Vec<_>>(),
        "asset_processor": find_processes("AssetProcessor").iter().map(|(pid, _)| *pid).collect::<Vec<_>>(),
        "build": build,
        "memory_free_mb": m.get("MemAvailable"),
        "memory_total_mb": m.get("MemTotal"),
        "memory_detail": ["MemFree", "MemAvailable", "Cached", "Buffers", "Shmem", "SwapTotal", "SwapFree", "Committed_AS"]
            .iter().map(|k| (k.to_string(), json!(m.get(*k)))).collect::<serde_json::Map<_, _>>(),
        "top_memory": top_memory(6),
        "compilers_running": compilers_running(),
        "editor_launch_known": p().launch_file.exists(),
    })
}

pub fn op_editor_start(args: &Value) -> Value {
    if !editor_processes().is_empty() {
        return json!({"ok": true, "note": "editor already running"});
    }
    if !flag(args, "force", false) {
        if setting_bool("laptop", "waitForBuilds", true) && !wait_for_compilers(int(args, "compiler_wait", 180).max(0) as u64) {
            return err("a build is still compiling; not opening the editor on top of it");
        }
        let free = *meminfo().get("MemAvailable").unwrap_or(&0) as i64;
        if free < int(args, "min_free_mb", 1800) {
            return json!({"ok": false, "error": format!("only {free} MB free; the editor needs about 1.8 GB"), "top_memory": top_memory(6)});
        }
    }
    let mut launch = launch_file();
    if !launch.is_object() {
        launch = default_editor_launch().unwrap_or(Value::Null);
    }
    if let (Some(engine_editor), true) = (project_engine_editor(), launch.is_object()) {
        let a = args_of(&launch);
        if let Some(first) = a.first() {
            if realpath(first) != realpath(&engine_editor) {
                log(&format!("the project uses {engine_editor}; not reusing the remembered {first}"));
                let mut env = launch.get("env").cloned().unwrap_or(json!({}));
                if let Some(e) = env.as_object_mut() {
                    e.remove("LD_LIBRARY_PATH");
                }
                let mut new_args = vec![engine_editor];
                new_args.extend(a.into_iter().skip(1));
                launch["args"] = json!(new_args);
                launch["env"] = env;
            }
        }
    }
    let other = text(args, "project_path");
    if !other.is_empty() && launch.is_object() {
        let other_path = fs::canonicalize(&other).unwrap_or_else(|_| PathBuf::from(&other));
        let root = fs::canonicalize(&p().projects_root).unwrap_or_else(|_| p().projects_root.clone());
        if other_path.parent() != Some(root.as_path()) || !other_path.join("project.json").is_file() {
            return err("not a project in the projects folder");
        }
        let mut a: Vec<String> = args_of(&launch).into_iter().filter(|x| !x.starts_with("--project-path")).collect();
        a.push(format!("--project-path={}", other_path.display()));
        launch["args"] = json!(a);
        launch["cwd"] = json!(other_path.display().to_string());
    }
    let a = args_of(&launch);
    if a.is_empty() {
        return err("don't know where the editor is yet: open it once from the Project Manager");
    }
    let env = session_env(launch.get("env").unwrap_or(&Value::Null));
    let cwd = launch.get("cwd").and_then(Value::as_str).filter(|c| !c.is_empty()).map(PathBuf::from).unwrap_or_else(|| p().project.clone());
    let mut child = match spawn(&own_scope(a), &cwd, &env, &p().home.join("editor_stdout.log"), false) {
        Ok(c) => c,
        Err(e) => return err(format!("couldn't start the editor: {e}")),
    };
    let pid = child.id();
    log(&format!("editor started, pid {pid}"));
    let wait = num(args, "wait_seconds", 0.0);
    let deadline = std::time::Instant::now() + Duration::from_secs_f64(wait.max(0.0));
    while wait > 0.0 && std::time::Instant::now() < deadline {
        if bridge_alive(5.0) {
            return json!({"ok": true, "pid": pid, "bridge": "ready"});
        }
        if let Ok(Some(status)) = child.try_wait() {
            return json!({"ok": false, "pid": pid, "error": format!("editor exited with code {}", status.code().unwrap_or(-1)),
                          "log": tail(&p().log_dir.join("Editor.log"), 40)});
        }
        std::thread::sleep(Duration::from_secs(1));
    }
    // Reaps it when it ends, so it never lingers as a zombie.
    ctx::spawn(move || {
        let _ = child.wait();
    });
    json!({"ok": true, "pid": pid, "bridge": if bridge_alive(5.0) { "ready" } else { "starting" }})
}

pub fn op_editor_stop(args: &Value) -> Value {
    let procs = editor_processes();
    if procs.is_empty() {
        return json!({"ok": true, "note": "editor not running"});
    }
    let mut saved = Value::Null;
    if flag(args, "save", true) && bridge_alive(10.0) {
        bridge_python("import azlmbr.legacy.general as g\nif g.is_in_game_mode(): g.exit_game_mode()\n", 30);
        std::thread::sleep(Duration::from_secs(1));
        let result = bridge_python("import azlmbr.legacy.general as g\ng.save_level()\nprint('saved')", 60);
        saved = json!(result.map(|r| r.to_string().contains("saved")).unwrap_or(false));
    }
    for (pid, _) in &procs {
        kill(*pid, libc::SIGTERM);
    }
    let deadline = std::time::Instant::now() + Duration::from_secs_f64(num(args, "timeout", 30.0).max(0.0));
    while std::time::Instant::now() < deadline && !editor_processes().is_empty() {
        std::thread::sleep(Duration::from_millis(500));
    }
    let mut forced = false;
    for (pid, _) in editor_processes() {
        forced = true;
        kill(pid, libc::SIGKILL);
    }
    log(&format!("editor stopped (saved={saved}, forced={forced})"));
    json!({"ok": true, "saved": saved, "forced": forced})
}

pub fn find_cmake() -> Option<String> {
    if let Some(c) = which("cmake") {
        return Some(c.display().to_string());
    }
    for pattern in ["~/.o3de/3rdParty/cmake-*/bin/cmake", "/var/opt/O3DE/*/cmake/runtime/bin/cmake"] {
        let mut hits = glob(pattern);
        hits.reverse();
        if let Some(h) = hits.first() {
            return Some(h.display().to_string());
        }
    }
    None
}

static BUILD_LOCK: Mutex<()> = Mutex::new(());

pub fn op_build(args: &Value) -> Value {
    let Ok(_guard) = BUILD_LOCK.try_lock() else {
        return json!({"ok": false, "error": "a build is already running", "build": ctx::get("build")});
    };
    let result = build(args);
    ctx::set("build", "running", json!(false));
    result
}

fn build(args: &Value) -> Value {
    let editor_open = !editor_processes().is_empty();
    let reopen = editor_open && flag(args, "reopen_editor", true);
    if editor_open {
        if !flag(args, "stop_editor", true) {
            return err("the editor is open; close it first or pass stop_editor");
        }
        op_editor_stop(&json!({"save": true}));
    }
    for (pid, _) in game_processes() {
        kill(pid, libc::SIGTERM);
    }
    let Some(cmake) = find_cmake() else { return err("cmake not found") };
    if ctx::engine_running_anywhere() {
        return err("the engine is building; game builds wait until it's done or stopped");
    }
    wait_for_compilers(240);
    let requested = if truthy(args.get("jobs")) { int(args, "jobs", 2) } else { settings_jobs() };
    let jobs = safe_jobs(requested).to_string();
    let (high, cap) = build_caps();
    let mut cmd = vec![cmake.clone(), "--build".into(), "build/linux".into(), "--target".into()];
    cmd.extend(p().build_targets());
    cmd.extend(["--config".into(), "profile".into(), "-j".into(), jobs]);
    let cmd = limited(cmd, high, cap);
    let started = now();
    {
        let mut s = ctx::state();
        s.build = json!({"running": true, "started": started, "finished": null, "exit_code": null});
    }
    let mut code = 0;
    if let Ok(mut out) = fs::File::create(&p().build_log) {
        use std::io::Write;
        if flag(args, "reconfigure", false) {
            let configure = vec![cmake.clone(), "-S".into(), ".".into(), "-B".into(), "build/linux".into()];
            log(&format!("configure: {}", configure.join(" ")));
            let _ = out.write_all(b"== configure ==\n");
            code = call(&configure, &p().project, None, &out);
        }
        if code == 0 {
            log(&format!("build started: {}", cmd.join(" ")));
            let _ = out.write_all(b"== build ==\n");
            code = call(&cmd, &p().project, None, &out);
        }
    }
    let finished = now();
    {
        let mut s = ctx::state();
        s.build = json!({"running": false, "started": started, "finished": finished, "exit_code": code});
    }
    log(&format!("build finished with code {code}"));
    let text_tail = tail(&p().build_log, 400);
    let lines: Vec<&str> = text_tail.lines().collect();
    let errors: Vec<&str> = lines.iter().copied().filter(|l| { let low = l.to_lowercase(); low.contains("error") && !low.contains("werror") }).take(40).collect();
    let steps = lines.iter().rev().find(|l| l.starts_with('[') && l.split(']').next().map(|s| s.contains('/')).unwrap_or(false))
        .map(|l| l.split(']').next().unwrap_or("").trim_start_matches('[').to_string());
    let seconds = (finished - started).round();
    let mut result = json!({"ok": code == 0, "exit_code": code, "seconds": seconds, "errors": errors,
                            "log_tail": lines[lines.len().saturating_sub(25)..].join("\n"), "steps": steps});
    if code == 0 && flag(args, "reconfigure", false) {
        save_stats(json!({"pending_rebuild": false, "built_gems": gem_names()}));
    }
    save_stats(json!({"last_build": {"ok": code == 0, "finished": finished, "steps": result["steps"], "seconds": seconds,
                                     "errors": errors.iter().take(5).collect::<Vec<_>>(), "log": p().build_log.display().to_string()}}));
    if reopen && code == 0 {
        result["editor"] = op_editor_start(&json!({"wait_seconds": num(args, "wait_seconds", 240.0)}));
    }
    result
}

pub fn op_game_start(args: &Value) -> Value {
    if !game_processes().is_empty() {
        return json!({"ok": true, "note": "game already running"});
    }
    let launcher = &p().launcher;
    if !launcher.exists() {
        return err(format!("no game launcher at {}; build first", launcher.display()));
    }
    let level = args.get("level").and_then(Value::as_str).unwrap_or("levels/level0/level0.spawnable");
    let mut cmd = vec![
        launcher.display().to_string(),
        format!("--project-path={}", p().project.display()),
        format!("--regset=/O3DE/Autoexec/ConsoleCommands/LoadLevel={level}"),
    ];
    if flag(args, "fullscreen", true) {
        cmd.push("--regset=/O3DE/Autoexec/ConsoleCommands/r_fullscreen=true".into());
    } else {
        cmd.push(format!("--regset=/O3DE/Autoexec/ConsoleCommands/r_width={}", int(args, "width", 1600)));
        cmd.push(format!("--regset=/O3DE/Autoexec/ConsoleCommands/r_height={}", int(args, "height", 900)));
    }
    for item in strings(args, "extra") {
        if item.starts_with("--regset=/NoclipProtocol/") {
            cmd.push(item);
        }
    }
    let launch = launch_file();
    let env = session_env(launch.get("env").unwrap_or(&Value::Null));
    let child = match spawn(&own_scope(cmd.clone()), &p().project, &env, &p().home.join("game_stdout.log"), false) {
        Ok(c) => c,
        Err(e) => return err(format!("couldn't start the game: {e}")),
    };
    let pid = child.id();
    log(&format!("game started, pid {pid}"));
    ctx::state().game = Some(child);
    std::thread::sleep(Duration::from_secs_f64(num(args, "wait_seconds", 3.0).max(0.0)));
    let exited = {
        let mut s = ctx::state();
        s.game.as_mut().and_then(|c| c.try_wait().ok().flatten()).map(|st| st.code().unwrap_or(-1))
    };
    if let Some(code) = exited {
        return json!({"ok": false, "error": format!("game exited with code {code}"), "log": tail(&p().home.join("game_stdout.log"), 40)});
    }
    json!({"ok": true, "pid": pid, "command": cmd})
}

pub fn op_game_stop(_args: &Value) -> Value {
    let procs = game_processes();
    for (pid, _) in &procs {
        kill(*pid, libc::SIGTERM);
    }
    std::thread::sleep(Duration::from_secs(2));
    for (pid, _) in game_processes() {
        kill(pid, libc::SIGKILL);
    }
    if let Some(mut c) = ctx::state().game.take() {
        let _ = c.try_wait();
    }
    json!({"ok": true, "stopped": procs.iter().map(|(pid, _)| *pid).collect::<Vec<_>>()})
}

pub fn op_crash_report(args: &Value) -> Value {
    let mut report = serde_json::Map::new();
    if which("coredumpctl").is_some() {
        let (out, _, _) = run(&["coredumpctl", "--no-pager", "list"], None, Duration::from_secs(30));
        let lines: Vec<String> = out.lines().filter(|l| l.contains("Editor") || l.contains("GameLauncher") || l.contains("AssetProc")).map(str::to_string).collect();
        report.insert("coredumps".into(), json!(lines[lines.len().saturating_sub(8)..]));
        if let Some(last) = lines.last() {
            if let Some(pid) = last.split_whitespace().find(|t| t.chars().all(|c| c.is_ascii_digit())) {
                let (info, _, _) = run(&["coredumpctl", "--no-pager", "info", pid], None, Duration::from_secs(60));
                report.insert("latest_info".into(), json!(info.lines().take(120).collect::<Vec<_>>().join("\n")));
            }
        }
    } else {
        report.insert("coredumps".into(), json!("coredumpctl not available"));
    }
    let mut backups = glob(&format!("{}/LogBackups/*", p().log_dir.display()));
    backups.sort_by(|a, b| mtime(a).partial_cmp(&mtime(b)).unwrap_or(std::cmp::Ordering::Equal));
    if let Some(latest) = backups.last() {
        let mut latest = latest.clone();
        if latest.is_dir() {
            let mut logs = glob(&format!("{}/*Editor*.log", latest.display()));
            if logs.is_empty() {
                logs = glob(&format!("{}/*.log", latest.display()));
            }
            if let Some(l) = logs.last() {
                latest = l.clone();
            }
        }
        report.insert("previous_editor_log".into(), json!({"file": latest.display().to_string(), "tail": tail(&latest, int(args, "lines", 80).max(1) as usize)}));
    }
    report.insert("editor_stdout_tail".into(), json!(tail(&p().home.join("editor_stdout.log"), 40)));
    Value::Object(report)
}

pub fn op_logs(args: &Value) -> Value {
    let which_log = args.get("which").and_then(Value::as_str).unwrap_or("editor");
    let files: Vec<(&str, PathBuf)> = vec![
        ("agent", p().agent_log.clone()),
        ("asset_processor", p().log_dir.join("AP_GUI.log")),
        ("build", p().build_log.clone()),
        ("editor", p().log_dir.join("Editor.log")),
        ("editor_stdout", p().home.join("editor_stdout.log")),
        ("game", p().log_dir.join("Game.log")),
        ("game_stdout", p().home.join("game_stdout.log")),
    ];
    match files.iter().find(|(k, _)| *k == which_log) {
        Some((_, f)) => json!({"file": f.display().to_string(), "tail": tail(f, int(args, "lines", 80).max(1) as usize)}),
        None => err(format!("unknown log; choose from {:?}", files.iter().map(|(k, _)| *k).collect::<Vec<_>>())),
    }
}

pub fn op_probe(_args: &Value) -> Value {
    let tools = ["firefox", "chromium", "google-chrome", "brave-browser", "microsoft-edge", "xdg-open", "cmake", "ninja", "ccache", "clang", "gcc", "coredumpctl", "nvidia-smi"];
    let found: serde_json::Map<String, Value> = tools.iter().map(|t| (t.to_string(), json!(which(t).map(|p| p.display().to_string())))).collect();
    let (qt, _, _) = run(&["python3", "-c", "import PySide6; print(PySide6.__version__)"], None, Duration::from_secs(30));
    let model = fs::read_to_string("/proc/cpuinfo").ok().and_then(|t| t.lines().find(|l| l.starts_with("model name")).and_then(|l| l.split_once(':')).map(|(_, v)| v.trim().to_string()));
    let gpu = if which("lspci").is_some() { run(&["lspci"], None, Duration::from_secs(20)).0 } else { String::new() };
    let (python, _, _) = run(&["python3", "--version"], None, Duration::from_secs(10));
    let mut st: libc::statvfs = unsafe { std::mem::zeroed() };
    let c = std::ffi::CString::new(p().project.display().to_string()).unwrap_or_default();
    let free_gb = if unsafe { libc::statvfs(c.as_ptr(), &mut st) } == 0 { (st.f_bavail as f64 * st.f_frsize as f64 / (1u64 << 30) as f64 * 10.0).round() / 10.0 } else { 0.0 };
    let swap: Vec<String> = fs::read_to_string("/proc/meminfo").unwrap_or_default().lines().filter(|l| l.starts_with("SwapTotal") || l.starts_with("SwapFree")).map(|l| format!("{l}\n")).collect();
    json!({"cpu": model, "cpu_count": std::thread::available_parallelism().map(|n| n.get()).ok(), "disk_free_gb": free_gb,
           "python": python.trim().trim_start_matches("Python ").to_string(), "system_pyside6": if qt.trim().is_empty() { Value::Null } else { json!(qt.trim()) },
           "tools": found, "gpu": gpu.lines().filter(|l| l.contains("VGA") || l.contains("3D")).collect::<Vec<_>>(), "swap": swap})
}

// ---------- watching the editor (every few seconds) ----------

const CRASH_HINTS: [&str; 10] = ["Segmentation fault", "SIGSEGV", "SIGABRT", "Assert", "assert", "Exception", "Fatal", "fatal error", "terminate called", "core dumped"];

pub fn last_level() -> Option<String> {
    let text = tail(&p().log_dir.join("Editor.log"), 4000);
    text.lines().rev().find(|l| l.contains("Successfully opened document")).and_then(|l| l.split_once("document")).map(|(_, rest)| {
        Path::new(rest.trim()).file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default()
    })
}

/// When the editor we saw goes away, decides whether it crashed and keeps what we know for the Hub.
pub fn check_editor_exit() {
    let pid = editor_processes().first().map(|(p, _)| *p);
    let old = {
        let mut s = ctx::state();
        let old = s.seen_editor;
        s.seen_editor = pid;
        old
    };
    let Some(old) = old else { return };
    if pid.is_some() {
        return;
    }
    let mut crashed = false;
    let mut dump = Value::Null;
    let have_dumps = which("coredumpctl").is_some();
    if have_dumps {
        let (out, _, code) = run(&["coredumpctl", "--no-pager", "list", &old.to_string()], None, Duration::from_secs(20));
        if code == 0 && out.contains(&old.to_string()) {
            crashed = true;
            dump = json!(out.lines().last().unwrap_or("").trim());
        }
    }
    let text_tail = tail(&p().log_dir.join("Editor.log"), 60);
    let lines: Vec<&str> = text_tail.lines().collect();
    let hints: Vec<String> = lines.iter().filter(|l| CRASH_HINTS.iter().any(|h| l.contains(h))).map(|l| l.trim().to_string()).collect();
    if !have_dumps && !hints.is_empty() && !lines[lines.len().saturating_sub(5)..].iter().any(|l| l.to_lowercase().contains("shutdown")) {
        crashed = true; // no crash list on this system: go by the end of the editor's log
    }
    if !crashed {
        return;
    }
    let mut during = Value::Null;
    for line in lines.iter().rev() {
        let low = line.to_lowercase();
        if low.contains("game mode") || low.contains("entering game") {
            during = json!("while entering Play");
            break;
        }
        if low.contains("saving") {
            during = json!("while saving");
            break;
        }
    }
    let level = last_level();
    let mut backup = Value::Null;
    if let Some(l) = &level {
        let mut folders = glob(&format!("{}/Levels/{l}/_autobackup/*", p().project.display()));
        folders.sort_by(|a, b| mtime(a).partial_cmp(&mtime(b)).unwrap_or(std::cmp::Ordering::Equal));
        if let Some(f) = folders.last() {
            backup = json!({"path": f.display().to_string(), "when": mtime(f)});
        }
    }
    let shown: Vec<String> = if hints.is_empty() { lines[lines.len().saturating_sub(6)..].iter().map(|s| s.to_string()).collect() } else { hints.clone() };
    let shown = shown[shown.len().saturating_sub(6)..].to_vec();
    save_stats(json!({"last_crash": {"when": now(), "pid": old, "during": during, "level": level, "dump": dump,
                                     "lines": shown, "log": p().log_dir.join("Editor.log").display().to_string(), "backup": backup}}));
    log(&format!("editor {old} closed unexpectedly ({})", during.as_str().unwrap_or("no hint")));
}

/// Keeps the highest memory the editor and the game reach (per run), for the Hub's cards.
pub fn track_memory() {
    let mut stats = load_stats();
    let mut changed = false;
    for (key, procs) in [("editor_memory", editor_processes()), ("game_memory", game_processes())] {
        if let Some((pid, _)) = procs.first() {
            let Some(mb) = rss_mb(*pid) else { continue };
            if mb == 0 {
                continue;
            }
            let old = stats.get(key).cloned().unwrap_or(json!({}));
            let same = old.get("pid").and_then(Value::as_i64) == Some(*pid as i64);
            let old_peak = old.get("peak_mb").and_then(Value::as_u64).unwrap_or(0);
            let peak = if same { mb.max(old_peak) } else { mb };
            if peak != old_peak || !same {
                stats.insert(key.into(), json!({"peak_mb": peak, "pid": pid, "when": now()}));
                changed = true;
            }
        }
    }
    if changed {
        let _ = fs::write(&p().stats_file, pretty(&Value::Object(stats), 2));
    }
}

/// Laptop setting "Pause asset processing while a game runs".
pub fn pause_assets_for_game() {
    let want = !game_processes().is_empty() && setting_bool("laptop", "pauseAssetsWhileGaming", false);
    let mut s = ctx::state();
    if want {
        for fragment in ["AssetProcessor", "AssetBuilder"] {
            for (pid, _) in find_processes(fragment) {
                if !s.paused_assets.contains(&pid) && kill(pid, libc::SIGSTOP) {
                    s.paused_assets.push(pid);
                    log(&format!("paused {fragment} {pid} while the game runs"));
                }
            }
        }
    } else if !s.paused_assets.is_empty() {
        for pid in s.paused_assets.drain(..) {
            kill(pid, libc::SIGCONT);
        }
        log("asset processing carries on (game closed)");
    }
}
