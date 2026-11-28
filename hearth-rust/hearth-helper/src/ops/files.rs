//! Looking into why the editor won't open, and moving files around inside the project.

use std::fs;
use std::os::unix::fs::MetadataExt;
use std::path::{Component, Path, PathBuf};
use std::time::Duration;

use serde_json::{json, Map, Value};

use crate::ctx::{p, user_home};
use crate::util::*;

pub fn op_engine_check(_args: &Value) -> Value {
    let mut out = Map::new();
    for root in ["/opt/hearth-engine", "/var/opt/hearth-engine", "/usr/share/hearth-engine"] {
        let path = Path::new(root);
        let mut info = Map::new();
        info.insert("exists".into(), json!(path.exists()));
        info.insert("is_symlink".into(), json!(fs::symlink_metadata(path).map(|m| m.file_type().is_symlink()).unwrap_or(false)));
        info.insert("real".into(), json!(fs::canonicalize(path).map(|p| p.display().to_string()).unwrap_or_else(|_| root.to_string())));
        if path.is_dir() {
            match fs::read_dir(path) {
                Ok(rd) => {
                    let mut names: Vec<String> = rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
                    names.sort();
                    names.truncate(60);
                    info.insert("top".into(), json!(names));
                }
                Err(e) => {
                    info.insert("top_error".into(), json!(e.to_string()));
                }
            }
            let ej = path.join("engine.json");
            info.insert("engine_json".into(), json!(ej.exists()));
            if ej.exists() {
                match fs::read_to_string(&ej).map_err(|e| e.to_string()).and_then(|t| serde_json::from_str::<Value>(&t).map_err(|e| e.to_string())) {
                    Ok(d) => {
                        info.insert("engine_json_name".into(), d.get("engine_name").cloned().unwrap_or(Value::Null));
                        info.insert("engine_json_version".into(), d.get("version").cloned().unwrap_or(Value::Null));
                        info.insert("engine_json_ok".into(), json!(true));
                    }
                    Err(e) => {
                        info.insert("engine_json_ok".into(), json!(false));
                        info.insert("engine_json_error".into(), json!(e.chars().take(200).collect::<String>()));
                    }
                }
                if let Ok(m) = fs::metadata(&ej) {
                    info.insert("engine_json_mode".into(), json!(format!("0o{:o}", m.mode() & 0o777)));
                    info.insert("engine_json_size".into(), json!(m.len()));
                }
            }
        }
        out.insert(root.into(), Value::Object(info));
    }
    let mut launchers = Vec::new();
    for d in ["/usr/share/applications".to_string(), user_home().join(".local/share/applications").display().to_string()] {
        let mut files = glob(&format!("{d}/*hearth*engine*.desktop"));
        files.extend(glob(&format!("{d}/*o3de*.desktop")));
        for f in files {
            if let Ok(t) = fs::read_to_string(&f) {
                let exec: Vec<String> = t.lines().filter(|l| l.starts_with("Exec=") || l.starts_with("Path=")).map(|l| format!("{l}\n")).collect();
                launchers.push(json!({"file": f.display().to_string(), "exec": exec}));
            }
        }
    }
    out.insert("launchers".into(), json!(launchers));
    let manifest = user_home().join(".o3de/o3de_manifest.json");
    if manifest.exists() {
        match read_json(&manifest) {
            Some(m) => {
                out.insert("o3de_manifest_engines".into(), m.get("engines").cloned().unwrap_or(Value::Null));
                out.insert("o3de_manifest_default_engines_folder".into(), m.get("default_engines_folder").cloned().unwrap_or(Value::Null));
            }
            None => {
                out.insert("o3de_manifest_error".into(), json!("can't read it"));
            }
        }
    }
    if let Some(engine) = read_json(&p().project_json).and_then(|v| v.get("engine").cloned()) {
        out.insert("project_engine".into(), engine);
    }
    if let Some(u) = read_json(&p().project.join("user/project.json")) {
        out.insert("user_project_json".into(), u);
    }
    let editor_log = p().log_dir.join("Editor.log");
    out.insert("editor_log_tail".into(), if editor_log.exists() { json!(tail(&editor_log, 20)) } else { Value::Null });
    Value::Object(out)
}

pub fn op_engine_python(_args: &Value) -> Value {
    let engine = Path::new("/var/opt/hearth-engine");
    let mut out = Map::new();
    out.insert("venvs".into(), json!(glob("~/.o3de/Python/venv/*").iter().map(|p| p.display().to_string()).collect::<Vec<_>>()));
    let python = engine.join("python/python.sh");
    out.insert("python_sh".into(), json!(python.exists()));
    let py = python.display().to_string();
    let (o, e, code) = run(
        &[&py, "-c", "import o3de, o3de.manifest as m; print(m.get_engine_json_data(engine_path='/var/opt/hearth-engine') is not None)"],
        Some(engine),
        Duration::from_secs(120),
    );
    out.insert("o3de_import".into(), json!({"code": code, "out": last_chars(&o, 1500), "err": last_chars(&e, 2500)}));
    let mut logs = glob("~/O3DE/HearthEngine/logs/*");
    logs.sort_by(|a, b| mtime(a).partial_cmp(&mtime(b)).unwrap_or(std::cmp::Ordering::Equal));
    let recent: Map<String, Value> = logs.iter().rev().take(3).rev().map(|f| (f.display().to_string(), json!(tail(f, 40)))).collect();
    out.insert("hub_logs".into(), Value::Object(recent));
    let launcher = which("hearth-engine");
    out.insert("launcher".into(), json!(launcher.as_ref().map(|p| p.display().to_string())));
    if let Some(l) = launcher {
        if let Ok(b) = fs::read(&l) {
            out.insert("launcher_head".into(), json!(String::from_utf8_lossy(&b[..b.len().min(1500)])));
        }
    }
    Value::Object(out)
}

/// A path inside the project, cleaned without touching the disk (so a not-yet-made folder works too).
fn inside(root: &Path, relative: &str) -> Option<PathBuf> {
    let mut out = root.to_path_buf();
    for c in Path::new(relative).components() {
        match c {
            Component::Normal(s) => out.push(s),
            Component::CurDir => {}
            Component::ParentDir => {
                if !out.pop() || !out.starts_with(root) {
                    return None;
                }
            }
            Component::RootDir | Component::Prefix(_) => return None,
        }
    }
    // A symlink inside the project must not lead outside it.
    let mut check = out.clone();
    while !check.exists() {
        if !check.pop() {
            break;
        }
    }
    let real = fs::canonicalize(&check).ok()?;
    real.starts_with(root).then_some(out)
}

fn root() -> PathBuf {
    fs::canonicalize(&p().project).unwrap_or_else(|_| p().project.clone())
}

/// project_copy {"from": "Voice", "to": "Assets/Sounds/Voice", "glob": "*.wav"}: copies files inside the project,
/// keeping the sub-folders, only what's new or changed.
pub fn op_project_copy(args: &Value) -> Value {
    let root = root();
    let (Some(src), Some(dst)) = (inside(&root, &text(args, "from")), inside(&root, &text(args, "to"))) else {
        return err("both folders must be inside the project");
    };
    if src == root || dst == root {
        return err("both folders must be inside the project");
    }
    let pattern = args.get("glob").and_then(Value::as_str).unwrap_or("*").to_string();
    let mut files = Vec::new();
    fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
        if let Ok(rd) = fs::read_dir(dir) {
            for e in rd.flatten() {
                match e.file_type() {
                    Ok(t) if t.is_dir() => walk(&e.path(), out),
                    Ok(t) if t.is_file() => out.push(e.path()),
                    _ => {}
                }
            }
        }
    }
    walk(&src, &mut files);
    let mut copied = 0;
    for f in files {
        let name = f.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        if !wildcard(&pattern, &name) {
            continue;
        }
        let Ok(rel) = f.strip_prefix(&src) else { continue };
        let target = dst.join(rel);
        if let Some(parent) = target.parent() {
            let _ = fs::create_dir_all(parent);
        }
        let newer = match (fs::metadata(&f), fs::metadata(&target)) {
            (Ok(a), Ok(b)) => b.mtime() < a.mtime() || b.len() != a.len(),
            _ => true,
        };
        if newer && fs::copy(&f, &target).is_ok() {
            copied += 1;
        }
    }
    json!({"ok": true, "copied": copied, "to": dst.display().to_string()})
}

/// unpack {"zip": "user/HearthAgent/drop/name.zip", "to": "."}: unpacks a zip into a folder inside the project,
/// keeping its folders. Refuses anything that would land outside the project.
pub fn op_unpack(args: &Value) -> Value {
    let root = root();
    let Some(zpath) = inside(&root, &text(args, "zip")).filter(|z| z.is_file()) else {
        return err("the zip and the folder must be inside the project");
    };
    let to = { let t = text(args, "to"); if t.is_empty() { ".".to_string() } else { t } };
    let Some(dst) = inside(&root, &to) else { return err("the zip and the folder must be inside the project") };
    let file = match fs::File::open(&zpath) {
        Ok(f) => f,
        Err(e) => return err(e.to_string()),
    };
    let mut archive = match zip::ZipArchive::new(file) {
        Ok(a) => a,
        Err(e) => return err(format!("not a zip: {e}")),
    };
    // Check every name first, so a bad one leaves nothing half unpacked.
    let mut plan = Vec::new();
    for i in 0..archive.len() {
        let Ok(entry) = archive.by_index(i) else { return err("couldn't read the zip") };
        let name = entry.name().to_string();
        let Some(target) = entry.enclosed_name().and_then(|n| inside(&root, &dst.join(n).strip_prefix(&root).unwrap_or(Path::new("")).display().to_string())) else {
            return err(format!("refused {name}: outside the project"));
        };
        if target == root && !entry.is_dir() {
            return err(format!("refused {name}: outside the project"));
        }
        plan.push((i, target, entry.is_dir()));
    }
    let mut written = 0;
    for (i, target, is_dir) in plan {
        if is_dir {
            let _ = fs::create_dir_all(&target);
            continue;
        }
        if let Some(parent) = target.parent() {
            let _ = fs::create_dir_all(parent);
        }
        let Ok(mut entry) = archive.by_index(i) else { continue };
        if let Ok(mut out) = fs::File::create(&target) {
            if std::io::copy(&mut entry, &mut out).is_ok() {
                written += 1;
            }
        }
    }
    if flag(args, "delete", true) {
        let _ = fs::remove_file(&zpath);
    }
    json!({"ok": true, "written": written, "to": dst.display().to_string()})
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nothing_escapes_the_project() {
        let dir = std::env::temp_dir().join(format!("hearth-helper-inside-{}", std::process::id()));
        fs::create_dir_all(dir.join("Voice")).unwrap();
        let root = fs::canonicalize(&dir).unwrap();
        assert_eq!(inside(&root, "Voice/new/a.wav"), Some(root.join("Voice/new/a.wav")));
        assert_eq!(inside(&root, "../outside"), None);
        assert_eq!(inside(&root, "Voice/../../outside"), None);
        assert_eq!(inside(&root, "/etc/passwd"), None);
        fs::remove_dir_all(&dir).unwrap();
    }
}
