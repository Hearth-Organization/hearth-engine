//! Hearth Engine helper: the background program that builds, runs and watches your games, so the Hub and Claude
//! can drive the editor even when it's closed, frozen or has crashed. The Rust version of
//! scripts/hearth/agent/hearth_agent.py (version 1.10), with the same requests and answers, for every game at once.
//!
//! How it's driven, as before:
//! - request files: `<game>/user/HearthAgent/inbox/<id>.json` = {"id", "token", "op", "args"} with that game's
//!   token (`<game>/user/HearthAgent/token`); the answer lands in `outbox/<id>.json`. Claude's requests go this way,
//!   and Hearth Settings › Claude's switches apply to them.
//! - the Hub's localhost API on 127.0.0.1:47830: POST /<op> with X-Hearth-Token (and X-Hearth-Project to name a
//!   game other than the current one); GET serves the Hub's page.
//!
//!     hearth-helper [--project PATH]

#![allow(clippy::type_complexity)]

mod ctx;
mod ops;
mod util;

use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::io::AsRawFd;
use std::os::unix::process::CommandExt;
use std::path::PathBuf;
use std::time::Duration;

use serde_json::{json, Value};

use crate::ctx::{config_dir, p, user_home, HTTP_PORT, VERSION};
use crate::util::*;

fn project_from_args() -> PathBuf {
    let args: Vec<String> = std::env::args().collect();
    if let Some(i) = args.iter().position(|a| a == "--project") {
        if let Some(v) = args.get(i + 1) {
            let v = if let Some(rest) = v.strip_prefix("~/") { user_home().join(rest) } else { PathBuf::from(v) };
            return fs::canonicalize(&v).unwrap_or(v);
        }
    }
    if let Some(v) = std::env::var_os("HEARTH_PROJECT").filter(|v| !v.is_empty()) {
        let v = PathBuf::from(v);
        return fs::canonicalize(&v).unwrap_or(v);
    }
    user_home().join("O3DE/Projects/NoclipProtocol")
}

fn random_token() -> String {
    let mut bytes = [0u8; 24];
    if let Ok(mut f) = fs::File::open("/dev/urandom") {
        let _ = f.read_exact(&mut bytes);
    }
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

/// This game's token (made the first time; only this user can read it).
fn token() -> String {
    let file = &p().token_file;
    if !file.exists() {
        let _ = fs::create_dir_all(&p().home);
        let _ = fs::write(file, random_token());
    }
    let _ = fs::set_permissions(file, fs::Permissions::from_mode(0o600));
    fs::read_to_string(file).map(|t| t.trim().to_string()).unwrap_or_default()
}

fn same(a: &str, b: &str) -> bool {
    // Every byte compared, so a wrong token takes as long as a right one.
    a.len() == b.len() && !a.is_empty() && a.bytes().zip(b.bytes()).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

/// Tells the Hub where the helper is: ~/.config/hearth-engine/agent.json (only this user can read it).
pub fn announce() {
    let dir = config_dir().join("hearth-engine");
    let _ = fs::create_dir_all(&dir);
    let path = dir.join("agent.json");
    let tmp = dir.join("agent.json.tmp");
    let games: Vec<String> = ctx::all().iter().map(|g| g.paths.project.display().to_string()).collect();
    let data = json!({"port": HTTP_PORT, "token": token(), "project": p().project.display().to_string(), "projects": games,
                      "pid": std::process::id(), "version": VERSION, "agent": "rust", "started": now()});
    if fs::write(&tmp, data.to_string()).is_ok() {
        let _ = fs::set_permissions(&tmp, fs::Permissions::from_mode(0o600));
        if let Err(e) = fs::rename(&tmp, &path) {
            log(&format!("agent.json not written: {e}"));
        }
    }
}

/// Puts Hearth Engine in the app menu (it opens the Hub), unless the installed engine has its own entry.
fn install_desktop_entry() {
    let apps = user_home().join(".local/share/applications");
    let entry = apps.join("hearth-engine.desktop");
    if which("hearth-engine").is_some() {
        if fs::read_to_string(&entry).map(|t| t.contains("xdg-open http://127.0.0.1")).unwrap_or(false) && fs::remove_file(&entry).is_ok() {
            log("app menu entry: removed the old web Hub entry (the engine's Hub is installed)");
        }
        return;
    }
    let icon = p().hub_dir.join("icon.svg");
    let text = format!(
        "[Desktop Entry]\nType=Application\nName=Hearth Engine\nComment=Build, play and edit your games\nExec=xdg-open http://127.0.0.1:{HTTP_PORT}/\nIcon={}\nCategories=Development;Game;\nTerminal=false\n",
        icon.display()
    );
    if fs::read_to_string(&entry).ok().as_deref() != Some(text.as_str()) {
        let _ = fs::create_dir_all(&apps);
        if fs::write(&entry, text).is_ok() {
            log("app menu entry: Hearth Engine");
        }
    }
}

/// Stopping or restarting the helper's service only stops the helper (the editor and the game survive).
fn fix_service_unit() {
    let unit = user_home().join(".config/systemd/user/hearth-engine-agent.service");
    let Ok(text) = fs::read_to_string(&unit) else { return };
    if text.contains("KillMode=process") {
        return;
    }
    if fs::write(&unit, text.replace("RestartSec=3\n", "RestartSec=3\nKillMode=process\n")).is_ok() {
        let _ = run(&["systemctl", "--user", "daemon-reload"], None, Duration::from_secs(30));
        log("service unit: KillMode=process (the editor and game survive helper restarts)");
    }
}

/// Starts this program again (a new build of it landed); same arguments.
pub fn restart() -> ! {
    let me = std::env::current_exe().unwrap_or_else(|_| PathBuf::from("hearth-helper"));
    let args: Vec<String> = std::env::args().skip(1).collect();
    let e = std::process::Command::new(&me).args(&args).exec();
    log(&format!("couldn't restart: {e}"));
    std::process::exit(1);
}

// ---------- the request files ----------

fn handle(path: PathBuf, token: String) {
    let request: Option<Value> = fs::read_to_string(&path).ok().and_then(|t| serde_json::from_str(&t).ok());
    let _ = fs::remove_file(&path);
    let Some(request) = request else { return };
    let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    let id = match request.get("id") {
        Some(Value::String(s)) if !s.is_empty() => s.clone(),
        Some(v) if !v.is_null() && truthy(Some(v)) => v.to_string(),
        _ => stem,
    };
    // An id becomes a file name: only plain characters.
    let id: String = id.chars().map(|c| if c.is_ascii_alphanumeric() || "-_.".contains(c) { c } else { '_' }).collect();
    let op = request.get("op").and_then(Value::as_str).unwrap_or("").to_string();
    let args = request.get("args").filter(|a| a.is_object()).cloned().unwrap_or(json!({}));
    let result = if !same(request.get("token").and_then(Value::as_str).unwrap_or(""), &token) {
        json!({"ok": false, "error": "bad token"})
    } else if let Some(f) = ops::find(&op) {
        match ops::claude_refusal(&op, &args) {
            Some(refusal) => refusal,
            None => ops::call(f, &args),
        }
    } else {
        let mut names = ops::names();
        names.sort();
        json!({"ok": false, "error": format!("unknown op; choose from {names:?}")})
    };
    let answer = json!({"id": id, "op": request.get("op"), "result": result, "answered": now()});
    let tmp = p().outbox.join(format!("{id}.tmp"));
    if fs::write(&tmp, pretty(&answer, 2)).is_ok() {
        let _ = fs::rename(&tmp, p().outbox.join(format!("{id}.json")));
    }
}

fn poll_inbox(game: &'static ctx::Project) {
    ctx::enter(game);
    let Ok(rd) = fs::read_dir(&game.paths.inbox) else { return };
    let mut files: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|f| f.extension().map(|e| e == "json").unwrap_or(false)).collect();
    files.sort();
    if files.is_empty() {
        return;
    }
    let token = token();
    for path in files {
        let claimed = path.with_extension("working");
        if fs::rename(&path, &claimed).is_err() {
            continue; // someone else has it
        }
        let t = token.clone();
        ctx::spawn(move || handle(claimed, t));
    }
}

/// The games in the projects folder that use the helper (they have a user/HearthAgent folder), plus the current one.
fn find_games() {
    let root = p().projects_root.clone();
    if let Ok(rd) = fs::read_dir(&root) {
        for e in rd.flatten() {
            let dir = e.path();
            if dir.join("project.json").is_file() && dir.join("user/HearthAgent").is_dir() {
                ctx::open(&dir);
            }
        }
    }
}

fn watch(game: &'static ctx::Project) {
    ctx::enter(game);
    let tasks: [(&str, fn()); 4] = [
        ("editor launch", ops::core::remember_editor_launch),
        ("memory tracking", ops::core::track_memory),
        ("crash check", ops::core::check_editor_exit),
        ("asset pause", ops::core::pause_assets_for_game),
    ];
    for (name, task) in tasks {
        if std::panic::catch_unwind(task).is_err() {
            log(&format!("{name}: stopped unexpectedly"));
        }
    }
    let _ = fs::write(&p().heartbeat, json!({"time": now(), "pid": std::process::id(), "version": VERSION, "agent": "rust"}).to_string());
}

// ---------- the Hub's localhost API ----------

fn respond(stream: &mut TcpStream, code: u16, kind: &str, body: &[u8]) {
    let reason = match code {
        200 => "OK",
        403 => "Forbidden",
        404 => "Not Found",
        _ => "Bad Request",
    };
    let head = format!(
        "HTTP/1.0 {code} {reason}\r\nContent-Type: {kind}\r\nCache-Control: no-store\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    let _ = stream.write_all(head.as_bytes());
    let _ = stream.write_all(body);
}

fn respond_json(stream: &mut TcpStream, code: u16, v: &Value) {
    respond(stream, code, "application/json", v.to_string().as_bytes());
}

fn serve(mut stream: TcpStream) {
    let _ = stream.set_read_timeout(Some(Duration::from_secs(30)));
    let mut reader = BufReader::new(match stream.try_clone() {
        Ok(s) => s,
        Err(_) => return,
    });
    let mut first = String::new();
    if reader.read_line(&mut first).is_err() {
        return;
    }
    let mut parts = first.split_whitespace();
    let (method, target) = (parts.next().unwrap_or("").to_string(), parts.next().unwrap_or("/").to_string());
    let mut headers: Vec<(String, String)> = Vec::new();
    loop {
        let mut line = String::new();
        if reader.read_line(&mut line).unwrap_or(0) == 0 || line.trim().is_empty() {
            break;
        }
        if let Some((k, v)) = line.split_once(':') {
            headers.push((k.trim().to_lowercase(), v.trim().to_string()));
        }
        if headers.len() > 100 {
            return;
        }
    }
    let header = |name: &str| headers.iter().find(|(k, _)| k == name).map(|(_, v)| v.clone()).unwrap_or_default();
    let host = header("host");
    if host != format!("127.0.0.1:{HTTP_PORT}") && host != format!("localhost:{HTTP_PORT}") {
        return respond_json(&mut stream, 403, &json!({"error": "bad host"}));
    }
    let default = ctx::default_project();
    ctx::enter(default);
    let path = target.split('?').next().unwrap_or("/").to_string();
    if method == "GET" {
        if path == "/cover" {
            let mut shots: Vec<PathBuf> = fs::read_dir(&p().shots).map(|rd| rd.flatten().map(|e| e.path()).filter(|f| f.extension().map(|e| e == "png").unwrap_or(false)).collect()).unwrap_or_default();
            shots.sort_by(|a, b| mtime(a).partial_cmp(&mtime(b)).unwrap_or(std::cmp::Ordering::Equal));
            return match shots.last().and_then(|s| fs::read(s).ok()) {
                Some(b) => respond(&mut stream, 200, "image/png", &b),
                None => respond_json(&mut stream, 404, &json!({"error": "no screenshots yet"})),
            };
        }
        let file = if path == "/" || path == "/index.html" { "index.html".to_string() } else { path.trim_start_matches('/').to_string() };
        let hub = fs::canonicalize(&p().hub_dir).unwrap_or_else(|_| p().hub_dir.clone());
        let resolved = fs::canonicalize(hub.join(&file)).ok().filter(|t| t.starts_with(&hub) && t.is_file());
        let Some(resolved) = resolved else { return respond_json(&mut stream, 404, &json!({"error": "not found"})) };
        let mut body = fs::read(&resolved).unwrap_or_default();
        let ext = resolved.extension().and_then(|e| e.to_str()).unwrap_or("");
        if ext == "html" {
            let t = String::from_utf8_lossy(&body).replace("__HEARTH_TOKEN__", &token());
            body = t.into_bytes();
        }
        let kind = match ext {
            "html" => "text/html; charset=utf-8",
            "js" => "text/javascript",
            "css" => "text/css",
            "png" => "image/png",
            "jpg" => "image/jpeg",
            "svg" => "image/svg+xml",
            "woff2" => "font/woff2",
            _ => "application/octet-stream",
        };
        return respond(&mut stream, 200, kind, &body);
    }
    if method != "POST" {
        return respond_json(&mut stream, 400, &json!({"error": "GET or POST"}));
    }
    // The current game's token opens every game in the projects folder (the same person on the same computer).
    if !same(&header("x-hearth-token"), &token()) {
        return respond_json(&mut stream, 403, &json!({"error": "forbidden"}));
    }
    let game = header("x-hearth-project");
    if !game.is_empty() {
        match ctx::known_game(&game) {
            Some(g) => ctx::enter(g),
            None => return respond_json(&mut stream, 404, &json!({"error": "that isn't a game in the projects folder"})),
        }
    }
    let op = path.trim_matches('/').rsplit('/').next().unwrap_or("").to_string();
    let Some(f) = ops::find(&op) else { return respond_json(&mut stream, 404, &json!({"error": "unknown op"})) };
    let length: usize = header("content-length").parse().unwrap_or(0).min(64 << 20);
    let mut raw = vec![0u8; length];
    let _ = reader.read_exact(&mut raw);
    let args: Value = serde_json::from_slice(&raw).ok().filter(Value::is_object).unwrap_or(json!({}));
    let result = ops::call(f, &args);
    respond_json(&mut stream, 200, &result);
}

fn start_http() {
    match TcpListener::bind(("127.0.0.1", HTTP_PORT)) {
        Ok(listener) => {
            log(&format!("hub API on http://127.0.0.1:{HTTP_PORT}/"));
            std::thread::spawn(move || {
                for stream in listener.incoming().flatten() {
                    std::thread::spawn(move || serve(stream));
                }
            });
        }
        Err(e) => log(&format!("hub API not started: {e}")),
    }
}

fn main() {
    if std::env::args().any(|a| a == "-h" || a == "--help") {
        println!("hearth-helper [--project PATH]: Hearth Engine's helper for every game in that game's projects folder.");
        return;
    }
    let first = ctx::open(&project_from_args());
    ctx::set_default(first);
    ctx::enter(first);
    let _ = fs::create_dir_all(&p().home);
    // One helper at a time (the Python one uses the same lock, so the two never run together).
    let lock = match fs::OpenOptions::new().create(true).truncate(false).write(true).open(&p().lock_file) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Can't open {}: {e}", p().lock_file.display());
            return;
        }
    };
    if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        println!("Hearth Engine agent is already running.");
        return;
    }
    let _ = token();
    log(&format!("helper {VERSION} (Rust) started, pid {}, project {}", std::process::id(), p().project.display()));
    // If an earlier helper paused asset processing for a game and then stopped, let it carry on.
    for fragment in ["AssetProcessor", "AssetBuilder"] {
        for (pid, _) in find_processes(fragment) {
            let stopped = fs::read_to_string(format!("/proc/{pid}/stat"))
                .ok()
                .and_then(|s| s.rsplit(')').next().and_then(|r| r.split_whitespace().next().map(|x| x == "T")))
                .unwrap_or(false);
            if stopped {
                kill(pid, libc::SIGCONT);
            }
        }
    }
    find_games();
    start_http();
    announce();
    fix_service_unit();
    install_desktop_entry();
    let me = std::env::current_exe().ok();
    let my_mtime = me.as_deref().and_then(mtime);
    let mut last_seen = 0.0;
    loop {
        let t = now();
        if t - last_seen > 5.0 {
            last_seen = t;
            ctx::enter(ctx::default_project());
            find_games();
            for game in ctx::all() {
                watch(game);
            }
            ctx::enter(ctx::default_project());
            let changed = me.as_deref().and_then(mtime) != my_mtime;
            let busy = ctx::all().iter().any(|g| g.state.lock().map(|s| s.build["running"] == true || s.engine["running"] == true).unwrap_or(true));
            if changed && !busy {
                std::thread::sleep(Duration::from_secs(1)); // let the new file finish landing
                log("helper program changed, restarting into the new one");
                drop(lock);
                restart();
            }
        }
        for game in ctx::all() {
            poll_inbox(game);
        }
        std::thread::sleep(Duration::from_millis(500));
    }
}

