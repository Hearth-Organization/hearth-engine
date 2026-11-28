//! Hearth Forge (the owner, Nov 26: "You need to have full control over hearth forge"): its service, its models,
//! making, what was made, and bringing a made model into the project's assets. Forge's own service answers on
//! 127.0.0.1:8191; Forge waits while a game is being played (Forge's own rule).

use std::fs;
use std::path::PathBuf;
use std::time::Duration;

use base64::Engine as _;
use serde_json::{json, Value};

use crate::ctx::{p, user_home};
use crate::util::*;

const FORGE_URL: &str = "http://127.0.0.1:8191";

fn helper() -> PathBuf {
    user_home().join(".local/share/hearth/forge/hearth-forge.py")
}

fn forge_dir() -> PathBuf {
    match std::env::var_os("HEARTH_FORGE_DIR") {
        Some(d) if !d.is_empty() => PathBuf::from(d),
        _ => user_home().join("Hearth/Forge"),
    }
}

/// (status code, answer) from a JSON service; code 0 when it isn't answering.
pub fn http_json(method: &str, url: &str, body: Option<&Value>, timeout: u64, bearer: Option<&str>) -> (u16, Value, Option<Vec<u8>>) {
    let agent = ureq::AgentBuilder::new().timeout(Duration::from_secs(timeout)).build();
    let mut req = agent.request(method, url);
    if let Some(key) = bearer.filter(|k| !k.is_empty()) {
        req = req.set("Authorization", &format!("Bearer {key}"));
    }
    let resp = match body {
        Some(b) => req.set("Content-Type", "application/json").send_string(&b.to_string()),
        None => req.call(),
    };
    let read = |r: ureq::Response| -> (u16, Value, Option<Vec<u8>>) {
        let status = r.status();
        let kind = r.content_type().to_string();
        let mut raw = Vec::new();
        let _ = std::io::Read::read_to_end(&mut r.into_reader(), &mut raw);
        if kind.contains("json") {
            (status, serde_json::from_slice(&raw).unwrap_or(json!({"error": "bad JSON"})), None)
        } else {
            (status, Value::Null, Some(raw))
        }
    };
    match resp {
        Ok(r) => read(r),
        Err(ureq::Error::Status(_, r)) => {
            let (status, v, raw) = read(r);
            let v = if v.is_null() { json!({"error": format!("HTTP {status}: {}", String::from_utf8_lossy(&raw.unwrap_or_default()).chars().take(200).collect::<String>())}) } else { v };
            (status, v, None)
        }
        Err(e) => (0, json!({"error": e.to_string()}), None),
    }
}

fn forge_http(method: &str, path: &str, body: Option<&Value>, timeout: u64) -> (u16, Value) {
    let (code, v, raw) = http_json(method, &format!("{FORGE_URL}{path}"), body, timeout, None);
    if code == 0 {
        return (0, json!({"error": format!("Hearth Forge isn't answering on 127.0.0.1:8191 ({})", v["error"].as_str().unwrap_or(""))}));
    }
    (code, if v.is_null() { json!(String::from_utf8_lossy(&raw.unwrap_or_default())) } else { v })
}

fn forge_cli(command: &[&str], timeout: u64) -> Value {
    let h = helper();
    if !h.is_file() {
        return err("Hearth Forge isn't set up on this PC: install it from the Hearth Store and open it once");
    }
    let hs = h.display().to_string();
    let mut cmd = vec!["python3", hs.as_str()];
    cmd.extend_from_slice(command);
    let (out, errs, code) = run(&cmd, None, Duration::from_secs(timeout));
    json!({"ok": code == 0, "code": code, "out": last_chars(&out, 4000), "err": last_chars(&errs, 2000)})
}

pub fn op_forge_status(_args: &Value) -> Value {
    let desktop = [PathBuf::from("/usr/share/applications/hearth-forge.desktop"), user_home().join(".local/share/applications/hearth-forge.desktop")];
    let app = which("hearth-forge").or_else(|| desktop.iter().find(|p| p.exists()).cloned()).map(|p| p.display().to_string());
    let (code, status) = forge_http("GET", "/status", None, 10);
    let has_helper = helper().is_file();
    json!({"ok": true, "installed": app.is_some() || has_helper, "app": app, "helper": has_helper,
           "service": if has_helper { forge_cli(&["status"], 30) } else { Value::Null },
           "answering": code == 200, "status": if code == 200 { status.clone() } else { Value::Null },
           "error": if code == 200 { Value::Null } else { status.get("error").cloned().unwrap_or(Value::Null) }})
}

pub fn op_forge_service(args: &Value) -> Value {
    match args.get("state").and_then(Value::as_str) {
        Some(s @ ("on" | "off")) => forge_cli(&[s], 60),
        _ => err("state is on or off"),
    }
}

fn plain_id(s: &str) -> bool {
    !s.is_empty() && s.chars().all(|c| c.is_alphanumeric() || "-._".contains(c))
}

pub fn op_forge_models(args: &Value) -> Value {
    for action in ["get", "remove"] {
        if truthy(args.get(action)) {
            let mid = text(args, action);
            if !plain_id(&mid) {
                return err("bad model id");
            }
            let (code, answer) = forge_http("POST", &format!("/models/{mid}/{action}"), Some(&json!({})), 30);
            return json!({"ok": code == 200, "answer": answer});
        }
    }
    let (code, status) = forge_http("GET", "/status", None, 10);
    json!({"ok": code == 200, "status": status})
}

pub fn op_forge_make(args: &Value) -> Value {
    let mut job = args.as_object().cloned().unwrap_or_default();
    job.remove("photos");
    let mut photos = Vec::new();
    for ph in strings(args, "photos") {
        let path = if let Some(rest) = ph.strip_prefix("~/") { user_home().join(rest) } else { PathBuf::from(&ph) };
        match fs::metadata(&path) {
            Ok(m) if m.is_file() && m.len() <= 32 << 20 => match fs::read(&path) {
                Ok(bytes) => photos.push(json!(base64::engine::general_purpose::STANDARD.encode(bytes))),
                Err(_) => return err(format!("photo not found or too big: {ph}")),
            },
            _ => return err(format!("photo not found or too big: {ph}")),
        }
    }
    if !photos.is_empty() {
        job.insert("photos".into(), json!(photos));
    }
    let (code, answer) = forge_http("POST", "/make", Some(&Value::Object(job)), 60);
    json!({"ok": code == 200, "answer": answer})
}

pub fn op_forge_made(args: &Value) -> Value {
    if truthy(args.get("stop")) {
        let name = text(args, "stop");
        if !plain_id(&name) {
            return err("bad name");
        }
        let (code, answer) = forge_http("POST", &format!("/made/{name}/stop"), Some(&json!({})), 30);
        return json!({"ok": code == 200, "answer": answer});
    }
    let (code, answer) = forge_http("GET", "/made", None, 15);
    json!({"ok": code == 200, "made": if code == 200 { answer.clone() } else { Value::Null },
           "error": if code == 200 { Value::Null } else { answer.get("error").cloned().unwrap_or(Value::Null) }})
}

/// forge_import {"name", "as": "Weapons/m4_carbine", "format": "glb"|"fbx"|"obj", "faces", "texture", "height"}:
/// exports the made model (Forge's own export, through Blender) into Assets/Models/Forge/<as>/ with its photo
/// and settings, where the Asset Processor picks it up.
pub fn op_forge_import(args: &Value) -> Value {
    let name = text(args, "name");
    let dest = { let d = text(args, "as"); if d.is_empty() { name.clone() } else { d } };
    if name.is_empty() || !name.chars().all(|c| c.is_alphanumeric() || c == '-') {
        return err("name is a made model's name");
    }
    if dest.is_empty() || dest.split('/').any(|s| s == "..") || dest.starts_with('/') || !dest.chars().all(|c| c.is_alphanumeric() || "-_/".contains(c)) {
        return err("as is a folder under Assets/Models/Forge (letters, digits, - _ /)");
    }
    let made = forge_dir().join(&name);
    if !made.join("model.glb").is_file() {
        return err(format!("{name} has no model.glb yet (still being made, or no such model)"));
    }
    let fmt = args.get("format").and_then(Value::as_str).unwrap_or("fbx");
    if !["glb", "fbx", "obj"].contains(&fmt) {
        return err("format is glb, fbx or obj");
    }
    let out_dir = p().project.join("Assets/Models/Forge").join(&dest);
    if let Err(e) = fs::create_dir_all(&out_dir) {
        return err(e.to_string());
    }
    let leaf = std::path::Path::new(&dest).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| name.clone());
    let out = out_dir.join(format!("{leaf}.{fmt}"));
    let mut extra: Vec<String> = Vec::new();
    for (key, flag_name) in [("faces", "--faces"), ("texture", "--texture"), ("height", "--height")] {
        if truthy(args.get(key)) {
            extra.push(flag_name.into());
            extra.push(text(args, key));
        }
    }
    let mut result = if fmt == "glb" && extra.is_empty() {
        match fs::copy(made.join("model.glb"), &out) {
            Ok(_) => json!({"ok": true}),
            Err(e) => err(e.to_string()),
        }
    } else {
        let m = made.display().to_string();
        let o = out.display().to_string();
        let mut cmd: Vec<&str> = vec!["export", &m, fmt, &o];
        cmd.extend(extra.iter().map(String::as_str));
        forge_cli(&cmd, 600)
    };
    for f in ["photo.png", "preview.png", "ask.json", "stats.json"] {
        if made.join(f).is_file() {
            let _ = fs::copy(made.join(f), out_dir.join(f));
        }
    }
    result["file"] = if out.exists() { json!(out.strip_prefix(&p().project).unwrap_or(&out).display().to_string()) } else { Value::Null };
    result
}
