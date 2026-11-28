//! Small things every operation uses: the log, reading the end of a file, processes, memory, settings, and how
//! heavy programs are started (their own systemd scope, a memory cap) so they can't take the computer down.

use std::collections::HashMap;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::fs::MetadataExt;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use serde_json::{json, Map, Value};

use crate::ctx::{config_dir, p};

pub fn now() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

pub fn mtime(path: &Path) -> Option<f64> {
    fs::metadata(path).ok().map(|m| m.mtime() as f64 + m.mtime_nsec() as f64 / 1e9)
}

/// "2026-11-28 10:15:00" in local time (from the C library, like Python's time.strftime).
pub fn stamp(format: &str) -> String {
    let t = unsafe { libc::time(std::ptr::null_mut()) };
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe { libc::localtime_r(&t, &mut tm) };
    let fmt = std::ffi::CString::new(format).unwrap_or_default();
    let mut buf = [0u8; 64];
    let n = unsafe { libc::strftime(buf.as_mut_ptr() as *mut libc::c_char, buf.len(), fmt.as_ptr(), &tm) };
    String::from_utf8_lossy(&buf[..n]).into_owned()
}

pub fn log(message: &str) {
    let line = format!("{} {message}\n", stamp("%Y-%m-%d %H:%M:%S"));
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(&p().agent_log) {
        let _ = f.write_all(line.as_bytes());
    }
}

/// The last `lines` lines of a file (reading at most its last 256 KB).
pub fn tail(path: &Path, lines: usize) -> String {
    let mut f = match fs::File::open(path) {
        Ok(f) => f,
        Err(e) => return format!("(can't read {}: {e})", path.display()),
    };
    let size = f.seek(SeekFrom::End(0)).unwrap_or(0);
    let _ = f.seek(SeekFrom::Start(size.saturating_sub(256 * 1024)));
    let mut raw = Vec::new();
    let _ = f.read_to_end(&mut raw);
    let text = String::from_utf8_lossy(&raw);
    let all: Vec<&str> = text.lines().collect();
    all[all.len().saturating_sub(lines)..].join("\n")
}

pub fn read_json(path: &Path) -> Option<Value> {
    serde_json::from_str(&fs::read_to_string(path).ok()?).ok()
}

pub fn pretty(v: &Value, indent: usize) -> String {
    let pad = " ".repeat(indent);
    let mut out = Vec::new();
    let formatter = serde_json::ser::PrettyFormatter::with_indent(pad.as_bytes());
    let mut ser = serde_json::Serializer::with_formatter(&mut out, formatter);
    serde::Serialize::serialize(v, &mut ser).ok();
    String::from_utf8(out).unwrap_or_default()
}

/// Writes through a temporary file, so a reader never sees half a file.
pub fn write_atomic(path: &Path, text: &str) -> std::io::Result<()> {
    let tmp = path.with_extension(format!(
        "{}.tmp",
        path.extension().and_then(|e| e.to_str()).unwrap_or("")
    ));
    fs::write(&tmp, text)?;
    fs::rename(&tmp, path)
}

pub fn which(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    std::env::split_paths(&path).map(|d| d.join(name)).find(|c| {
        fs::metadata(c).map(|m| m.is_file() && m.mode() & 0o111 != 0).unwrap_or(false)
    })
}

/// Glob for simple patterns: `*` and `?` inside one path segment, `~` at the start. Sorted.
pub fn glob(pattern: &str) -> Vec<PathBuf> {
    let pattern = if let Some(rest) = pattern.strip_prefix("~/") {
        format!("{}/{rest}", crate::ctx::user_home().display())
    } else {
        pattern.to_string()
    };
    let absolute = pattern.starts_with('/');
    let mut current: Vec<PathBuf> = vec![if absolute { PathBuf::from("/") } else { PathBuf::from(".") }];
    for seg in pattern.split('/').filter(|s| !s.is_empty()) {
        let mut next = Vec::new();
        for base in &current {
            if seg.contains('*') || seg.contains('?') {
                if let Ok(rd) = fs::read_dir(base) {
                    for e in rd.flatten() {
                        let name = e.file_name().to_string_lossy().into_owned();
                        if !name.starts_with('.') && wildcard(seg, &name) {
                            next.push(base.join(name));
                        }
                    }
                }
            } else {
                let c = base.join(seg);
                if c.exists() || fs::symlink_metadata(&c).is_ok() {
                    next.push(c);
                }
            }
        }
        current = next;
    }
    current.sort();
    current
}

pub fn wildcard(pattern: &str, text: &str) -> bool {
    let p: Vec<char> = pattern.chars().collect();
    let t: Vec<char> = text.chars().collect();
    let (mut pi, mut ti, mut star, mut mark) = (0usize, 0usize, None, 0usize);
    while ti < t.len() {
        if pi < p.len() && (p[pi] == '?' || p[pi] == t[ti]) {
            pi += 1;
            ti += 1;
        } else if pi < p.len() && p[pi] == '*' {
            star = Some(pi);
            mark = ti;
            pi += 1;
        } else if let Some(s) = star {
            pi = s + 1;
            mark += 1;
            ti = mark;
        } else {
            return false;
        }
    }
    while pi < p.len() && p[pi] == '*' {
        pi += 1;
    }
    pi == p.len()
}

// ---------- processes ----------

pub fn uid() -> u32 {
    unsafe { libc::getuid() }
}

/// (pid, command line) of this user's processes whose program name contains `fragment`.
pub fn find_processes(fragment: &str) -> Vec<(i32, Vec<String>)> {
    let mut found = Vec::new();
    let me = uid();
    if let Ok(rd) = fs::read_dir("/proc") {
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            let Ok(pid) = name.parse::<i32>() else { continue };
            let Ok(raw) = fs::read(format!("/proc/{pid}/cmdline")) else { continue };
            if raw.is_empty() {
                continue;
            }
            let args: Vec<String> =
                raw.split(|b| *b == 0).filter(|a| !a.is_empty()).map(|a| String::from_utf8_lossy(a).into_owned()).collect();
            let Some(first) = args.first() else { continue };
            let program = Path::new(first).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            let owner = fs::metadata(format!("/proc/{pid}")).map(|m| m.uid()).unwrap_or(u32::MAX);
            if program.contains(fragment) && owner == me {
                found.push((pid, args));
            }
        }
    }
    found.sort_by_key(|(pid, _)| *pid);
    found
}

pub fn basename(s: &str) -> String {
    Path::new(s).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
}

/// The project a running engine program works on: its --project-path, else the folder it runs in.
fn project_of(pid: i32, args: &[String]) -> Option<PathBuf> {
    let mut given: Option<String> = None;
    for (i, a) in args.iter().enumerate() {
        if let Some(v) = a.strip_prefix("--project-path=") {
            given = Some(v.to_string());
        } else if a == "--project-path" {
            given = args.get(i + 1).cloned();
        }
    }
    let path = match given {
        Some(g) => PathBuf::from(g),
        None => fs::read_link(format!("/proc/{pid}/cwd")).ok()?,
    };
    Some(fs::canonicalize(&path).unwrap_or(path))
}

fn ours(pid: i32, args: &[String]) -> bool {
    let me = fs::canonicalize(&p().project).unwrap_or_else(|_| p().project.clone());
    match project_of(pid, args) {
        Some(theirs) => theirs == me || (theirs.starts_with(&me) && !args.iter().any(|a| a.starts_with("--project-path"))),
        None => false,
    }
}

/// The editors open on this game (each game's editor is its own program; the tabs show them in one window).
pub fn editor_processes() -> Vec<(i32, Vec<String>)> {
    find_processes("Editor").into_iter().filter(|(pid, a)| basename(&a[0]) == "Editor" && ours(*pid, a)).collect()
}

pub fn game_processes() -> Vec<(i32, Vec<String>)> {
    find_processes(&format!("{}.GameLauncher", p().name))
}

pub fn kill(pid: i32, signal: i32) -> bool {
    unsafe { libc::kill(pid, signal) == 0 }
}

pub fn rss_mb(pid: i32) -> Option<u64> {
    let text = fs::read_to_string(format!("/proc/{pid}/status")).ok()?;
    text.lines().find(|l| l.starts_with("VmRSS")).and_then(|l| l.split_whitespace().nth(1)).and_then(|k| k.parse::<u64>().ok()).map(|k| k / 1024)
}

pub fn meminfo() -> HashMap<String, u64> {
    let mut m = HashMap::new();
    if let Ok(text) = fs::read_to_string("/proc/meminfo") {
        for l in text.lines() {
            if let Some((k, rest)) = l.split_once(':') {
                if let Some(v) = rest.split_whitespace().next().and_then(|v| v.parse::<u64>().ok()) {
                    m.insert(k.to_string(), v / 1024);
                }
            }
        }
    }
    m
}

/// The programs using the most memory right now: [[MB, name], ...], biggest first.
pub fn top_memory(count: usize) -> Value {
    let mut totals: HashMap<String, u64> = HashMap::new();
    if let Ok(rd) = fs::read_dir("/proc") {
        for e in rd.flatten() {
            let Ok(pid) = e.file_name().to_string_lossy().parse::<i32>() else { continue };
            let Ok(name) = fs::read_to_string(format!("/proc/{pid}/comm")) else { continue };
            if let Some(mb) = rss_mb(pid) {
                *totals.entry(name.trim().to_string()).or_default() += mb;
            }
        }
    }
    let mut list: Vec<(u64, String)> = totals.into_iter().map(|(n, mb)| (mb, n)).collect();
    list.sort_by(|a, b| b.cmp(a));
    Value::Array(list.into_iter().take(count).map(|(mb, n)| json!([mb, n])).collect())
}

pub fn compilers_running() -> usize {
    ["clang", "cc1plus", "ninja", "ld.mold", "mold"].iter().map(|f| find_processes(f).len()).sum()
}

/// Waits until no compiler or linker is running (a stopped build's last jobs can keep going for a minute).
pub fn wait_for_compilers(timeout: u64) -> bool {
    let deadline = std::time::Instant::now() + Duration::from_secs(timeout);
    while std::time::Instant::now() < deadline {
        if compilers_running() == 0 {
            return true;
        }
        std::thread::sleep(Duration::from_secs(2));
    }
    false
}

/// How many compile jobs fit in the memory that's free right now, keeping room for the desktop.
pub fn safe_jobs(requested: i64) -> i64 {
    let available = *meminfo().get("MemAvailable").unwrap_or(&0) as i64;
    let fit = ((available - 1800) / 900).max(1);
    requested.min(fit).max(1)
}

// ---------- Hearth Engine settings (read only: the helper never writes what the owner chose) ----------

/// ~/.config/hearth-engine/settings.ini, as {section: {key: value}} (keys as written).
pub fn settings_ini() -> HashMap<String, HashMap<String, String>> {
    parse_ini(&fs::read_to_string(config_dir().join("hearth-engine").join("settings.ini")).unwrap_or_default())
}

pub fn parse_ini(text: &str) -> HashMap<String, HashMap<String, String>> {
    let mut out: HashMap<String, HashMap<String, String>> = HashMap::new();
    let mut section = String::new();
    for raw in text.lines() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            section = line[1..line.len() - 1].trim().to_string();
            continue;
        }
        if let Some((k, v)) = line.split_once('=').or_else(|| line.split_once(':')) {
            out.entry(section.clone()).or_default().insert(k.trim().to_string(), v.trim().to_string());
        }
    }
    out
}

pub fn ini_get(ini: &HashMap<String, HashMap<String, String>>, section: &str, key: &str) -> Option<String> {
    let s = ini.get(section)?;
    s.get(key).cloned().or_else(|| s.iter().find(|(k, _)| k.eq_ignore_ascii_case(key)).map(|(_, v)| v.clone()))
}

pub fn setting_bool(section: &str, key: &str, default: bool) -> bool {
    match ini_get(&settings_ini(), section, key) {
        Some(v) => matches!(v.trim().to_lowercase().as_str(), "true" | "1" | "yes" | "on"),
        None => default,
    }
}

pub fn setting_int(section: &str, key: &str) -> Option<i64> {
    ini_get(&settings_ini(), section, key).and_then(|v| v.trim().parse().ok())
}

/// Hearth Settings › Performance › Build jobs at once: a number, or Auto (0): all the cores, which safe_jobs then
/// trims to the free memory.
pub fn settings_jobs() -> i64 {
    match setting_int("laptop", "buildJobsChoice") {
        None => setting_int("laptop", "buildJobs").unwrap_or(2),
        Some(c) if c > 0 => c,
        Some(_) => std::thread::available_parallelism().map(|n| n.get() as i64).unwrap_or(2),
    }
}

/// A heavy job in its own systemd scope with a memory cap, so past the cap that one job slows or fails instead of
/// the whole computer freezing ("Never let builds freeze the laptop", on unless the owner turns it off).
pub fn limited(cmd: Vec<String>, high_mb: u64, max_mb: u64) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    if which("systemd-run").is_some() && setting_bool("laptop", "limitBuildMemory", true) {
        for a in ["systemd-run", "--user", "--scope", "--quiet", "--collect"] {
            out.push(a.into());
        }
        out.push("-p".into());
        out.push(format!("MemoryHigh={high_mb}M"));
        out.push("-p".into());
        out.push(format!("MemoryMax={max_mb}M"));
        out.push("-p".into());
        out.push("MemorySwapMax=1G".into());
        out.push("--".into());
    }
    out.extend(["nice".to_string(), "-n".into(), "15".into()]);
    out.extend(cmd);
    out
}

pub fn build_caps() -> (u64, u64) {
    let total = *meminfo().get("MemTotal").unwrap_or(&8000);
    ((total as f64 * 0.45) as u64, (total as f64 * 0.6) as u64)
}

/// A program in its own systemd scope, so restarting the helper never takes it down.
pub fn own_scope(cmd: Vec<String>) -> Vec<String> {
    if which("systemd-run").is_some() {
        let mut out: Vec<String> =
            ["systemd-run", "--user", "--scope", "--quiet", "--collect", "--"].iter().map(|s| s.to_string()).collect();
        out.extend(cmd);
        out
    } else {
        cmd
    }
}

/// The environment for graphical programs: what the editor last ran with, else sane defaults.
pub fn session_env(extra: &Value) -> HashMap<String, String> {
    let mut env: HashMap<String, String> = std::env::vars().collect();
    if let Some(m) = extra.as_object() {
        for (k, v) in m {
            if let Some(s) = v.as_str() {
                env.insert(k.clone(), s.to_string());
            }
        }
    }
    // Engine programs find their own libraries next to them; a library path from another engine would load its.
    env.remove("LD_LIBRARY_PATH");
    env.entry("XDG_RUNTIME_DIR".into()).or_insert_with(|| format!("/run/user/{}", uid()));
    if !env.contains_key("WAYLAND_DISPLAY") {
        let dir = env["XDG_RUNTIME_DIR"].clone();
        if let Some(s) = glob(&format!("{dir}/wayland-[0-9]")).first().or(glob(&format!("{dir}/wayland-?")).first()) {
            env.insert("WAYLAND_DISPLAY".into(), basename(&s.to_string_lossy()));
        }
    }
    env.entry("DISPLAY".into()).or_insert_with(|| ":0".into());
    env.insert("QT_QPA_PLATFORM".into(), "xcb".into()); // Vulkan needs X11/XWayland on this machine
    env
}

/// Starts a program in its own session (it outlives the helper), output appended to `log`.
pub fn spawn(cmd: &[String], cwd: &Path, env: &HashMap<String, String>, log_file: &Path, truncate: bool) -> std::io::Result<std::process::Child> {
    let out = fs::OpenOptions::new().create(true).append(!truncate).write(true).truncate(truncate).open(log_file)?;
    let err = out.try_clone()?;
    let mut c = Command::new(&cmd[0]);
    c.args(&cmd[1..]).current_dir(cwd).env_clear().envs(env).stdin(Stdio::null()).stdout(out).stderr(err);
    unsafe {
        c.pre_exec(|| {
            libc::setsid();
            Ok(())
        });
    }
    c.spawn()
}

/// Runs a command to the end, output appended to an open log. Returns its exit code (-1 if it couldn't start).
pub fn call(cmd: &[String], cwd: &Path, env: Option<&HashMap<String, String>>, out: &fs::File) -> i32 {
    let mut c = Command::new(&cmd[0]);
    c.args(&cmd[1..]).current_dir(cwd).stdin(Stdio::null());
    if let Some(e) = env {
        c.env_clear().envs(e);
    }
    if let (Ok(o), Ok(e)) = (out.try_clone(), out.try_clone()) {
        c.stdout(o).stderr(e);
    }
    match c.status() {
        Ok(s) => s.code().unwrap_or(-1),
        Err(e) => {
            let mut o = out;
            let _ = writeln!(o, "couldn't start {}: {e}", cmd[0]);
            -1
        }
    }
}

/// Output of a command (stdout, stderr, exit code), with a time limit.
pub fn run(cmd: &[&str], cwd: Option<&Path>, timeout: Duration) -> (String, String, i32) {
    let mut c = Command::new(cmd[0]);
    c.args(&cmd[1..]).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    if let Some(d) = cwd {
        c.current_dir(d);
    }
    let mut child = match c.spawn() {
        Ok(ch) => ch,
        Err(e) => return (String::new(), e.to_string(), -1),
    };
    let mut so = child.stdout.take();
    let mut se = child.stderr.take();
    let out_t = std::thread::spawn(move || {
        let mut s = String::new();
        if let Some(o) = so.as_mut() {
            let mut b = Vec::new();
            let _ = o.read_to_end(&mut b);
            s = String::from_utf8_lossy(&b).into_owned();
        }
        s
    });
    let err_t = std::thread::spawn(move || {
        let mut s = String::new();
        if let Some(o) = se.as_mut() {
            let mut b = Vec::new();
            let _ = o.read_to_end(&mut b);
            s = String::from_utf8_lossy(&b).into_owned();
        }
        s
    });
    let deadline = std::time::Instant::now() + timeout;
    let code = loop {
        match child.try_wait() {
            Ok(Some(s)) => break s.code().unwrap_or(-1),
            Ok(None) if std::time::Instant::now() < deadline => std::thread::sleep(Duration::from_millis(50)),
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                break -9;
            }
        }
    };
    (out_t.join().unwrap_or_default(), err_t.join().unwrap_or_default(), code)
}

pub fn last_chars(s: &str, n: usize) -> String {
    let chars: Vec<char> = s.chars().collect();
    chars[chars.len().saturating_sub(n)..].iter().collect()
}

// ---------- the Hub's numbers (stats.json) ----------

pub fn load_stats() -> Map<String, Value> {
    read_json(&p().stats_file).and_then(|v| v.as_object().cloned()).unwrap_or_default()
}

pub fn save_stats(values: Value) {
    let mut stats = load_stats();
    if let Some(m) = values.as_object() {
        for (k, v) in m {
            stats.insert(k.clone(), v.clone());
        }
    }
    let _ = fs::write(&p().stats_file, pretty(&Value::Object(stats), 2));
}

// ---------- reading arguments the way the Python helper did ----------

pub fn truthy(v: Option<&Value>) -> bool {
    match v {
        None | Some(Value::Null) => false,
        Some(Value::Bool(b)) => *b,
        Some(Value::Number(n)) => n.as_f64().map(|f| f != 0.0).unwrap_or(false),
        Some(Value::String(s)) => !s.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Object(o)) => !o.is_empty(),
    }
}

/// `args.get(key, default)` read as a yes/no (a missing key is the default; a present one is its truthiness).
pub fn flag(args: &Value, key: &str, default: bool) -> bool {
    match args.get(key) {
        None => default,
        v => truthy(v),
    }
}

pub fn num(args: &Value, key: &str, default: f64) -> f64 {
    match args.get(key) {
        Some(Value::Number(n)) => n.as_f64().unwrap_or(default),
        Some(Value::String(s)) => s.trim().parse().unwrap_or(default),
        Some(Value::Bool(b)) => {
            if *b {
                1.0
            } else {
                0.0
            }
        }
        _ => default,
    }
}

pub fn int(args: &Value, key: &str, default: i64) -> i64 {
    num(args, key, default as f64) as i64
}

pub fn text(args: &Value, key: &str) -> String {
    match args.get(key) {
        Some(Value::String(s)) => s.clone(),
        Some(Value::Null) | None => String::new(),
        Some(v) => v.to_string(),
    }
}

pub fn strings(args: &Value, key: &str) -> Vec<String> {
    args.get(key)
        .and_then(Value::as_array)
        .map(|a| a.iter().map(|v| v.as_str().map(str::to_string).unwrap_or_else(|| v.to_string())).collect())
        .unwrap_or_default()
}

pub fn err(message: impl Into<String>) -> Value {
    json!({"ok": false, "error": message.into()})
}

pub fn gem_names() -> Vec<String> {
    read_json(&p().project_json)
        .and_then(|v| v.get("gem_names").cloned())
        .and_then(|v| v.as_array().cloned())
        .map(|a| a.iter().filter_map(|x| x.as_str().map(str::to_string)).collect())
        .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wildcards() {
        assert!(wildcard("*Editor*.log", "Editor_2026.log"));
        assert!(wildcard("wayland-?", "wayland-0"));
        assert!(!wildcard("wayland-?", "wayland-0.lock"));
        assert!(wildcard("cmake-*", "cmake-3.31.0-linux"));
    }

    #[test]
    fn reads_qt_settings() {
        let ini = parse_ini("[laptop]\nbuildJobsChoice=0\nlimitBuildMemory=false\n\n[voice]\nserver=10.0.0.5\napiKey=abc\n");
        assert_eq!(ini_get(&ini, "laptop", "buildJobsChoice").as_deref(), Some("0"));
        assert_eq!(ini_get(&ini, "voice", "apikey").as_deref(), Some("abc"));
        assert_eq!(ini_get(&ini, "voice", "missing"), None);
    }

    #[test]
    fn python_style_arguments() {
        let a = json!({"force": 1, "jobs": "4", "save": false, "name": "Level0", "list": ["a", "b"]});
        assert!(flag(&a, "force", false));
        assert!(!flag(&a, "save", true));
        assert!(flag(&a, "missing", true));
        assert_eq!(int(&a, "jobs", 2), 4);
        assert_eq!(text(&a, "name"), "Level0");
        assert_eq!(strings(&a, "list"), vec!["a", "b"]);
    }
}
