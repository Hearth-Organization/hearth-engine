//! Safe saving for games and tools (the owner's rule: settings and saves are always remembered and never change by
//! themselves).
//!
//! - **Atomic**: a save is written to a new file, flushed to the disk, then swapped in. A crash or power cut leaves
//!   the old save or the new one, never half of one.
//! - **A backup**: the save before is kept as `name.bak`; if the main file is broken, the backup is used.
//! - **Never defaults over a broken file**: when both are broken, loading says so and saving refuses to replace
//!   them (the broken file is kept aside as `name.broken-<time>` only when asked).
//! - **Merge on write**: saving changes only the keys given, so two parts of a game saving different settings
//!   never undo each other.
//! - **Slots**: numbered save slots with a summary each (for a load screen).

use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};

use serde_json::{Map, Value};

#[derive(Debug, Clone, PartialEq)]
pub enum Loaded {
    /// No file yet (a first run): defaults are right.
    Missing,
    Good(Value),
    /// The main file was broken; this came from the backup.
    FromBackup(Value),
    /// Both are broken. Nothing will be saved over them until `set_aside` is called.
    Broken(String),
}

fn with_ext(p: &Path, ext: &str) -> PathBuf {
    let mut s = p.as_os_str().to_owned();
    s.push(ext);
    PathBuf::from(s)
}

pub fn backup_path(p: &Path) -> PathBuf {
    with_ext(p, ".bak")
}

fn read_json(p: &Path) -> Result<Option<Value>, String> {
    match fs::read(p) {
        Ok(b) if b.iter().all(|c| c.is_ascii_whitespace()) => Err(format!("{} is empty", p.display())),
        Ok(b) => serde_json::from_slice(&b).map(Some).map_err(|e| format!("{} is broken: {e}", p.display())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(format!("can't read {}: {e}", p.display())),
    }
}

pub fn load(p: &Path) -> Loaded {
    match read_json(p) {
        Ok(Some(v)) => Loaded::Good(v),
        Ok(None) => match read_json(&backup_path(p)) {
            Ok(Some(v)) => Loaded::FromBackup(v),
            _ => Loaded::Missing,
        },
        Err(main) => match read_json(&backup_path(p)) {
            Ok(Some(v)) => Loaded::FromBackup(v),
            Ok(None) => Loaded::Broken(main),
            Err(bak) => Loaded::Broken(format!("{main}; {bak}")),
        },
    }
}

/// Writes bytes so that the file is either the old one or the new one, never half.
pub fn write_atomic(p: &Path, bytes: &[u8]) -> Result<(), String> {
    let dir = p.parent().filter(|d| !d.as_os_str().is_empty()).unwrap_or(Path::new("."));
    fs::create_dir_all(dir).map_err(|e| format!("can't make {}: {e}", dir.display()))?;
    let tmp = with_ext(p, &format!(".tmp-{}", std::process::id()));
    let result = (|| -> std::io::Result<()> {
        let mut f = OpenOptions::new().write(true).create(true).truncate(true).open(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
        drop(f);
        // Keep the one before as the backup (copied, so the main file is never missing for a moment), unless it's
        // broken: a broken file never replaces a good backup.
        if matches!(read_json(p), Ok(Some(_))) {
            fs::copy(p, backup_path(p))?;
            if let Ok(b) = File::open(backup_path(p)) {
                let _ = b.sync_all();
            }
        }
        fs::rename(&tmp, p)?;
        if let Ok(d) = File::open(dir) {
            let _ = d.sync_all();
        }
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    result.map_err(|e| format!("can't save {}: {e}", p.display()))
}

/// Deep-merges `changes` into `base`: records merge key by key; anything else is replaced. A `null` in the changes
/// removes that key.
pub fn merge(base: &mut Value, changes: &Value) {
    match (base, changes) {
        (Value::Object(b), Value::Object(c)) => {
            for (k, v) in c {
                if v.is_null() {
                    b.remove(k);
                } else if let Some(old) = b.get_mut(k) {
                    merge(old, v);
                } else {
                    b.insert(k.clone(), v.clone());
                }
            }
        }
        (b, c) => *b = c.clone(),
    }
}

/// Saves only `changes` into the file (merged with what's there). Refuses when the file and its backup are both
/// broken, so a broken file is never replaced by something that has lost the rest.
pub fn save_changes(p: &Path, changes: &Value) -> Result<Value, String> {
    let mut current = match load(p) {
        Loaded::Missing => Value::Object(Map::new()),
        Loaded::Good(v) | Loaded::FromBackup(v) => v,
        Loaded::Broken(why) => return Err(format!("not saving: {why}. Nothing was changed")),
    };
    merge(&mut current, changes);
    let text = serde_json::to_string_pretty(&current).map_err(|e| e.to_string())?;
    write_atomic(p, text.as_bytes())?;
    Ok(current)
}

/// Saves a whole value (a game save). Same refusal as `save_changes` for broken files unless `replace_broken`.
pub fn save_whole(p: &Path, value: &Value, replace_broken: bool) -> Result<(), String> {
    if let Loaded::Broken(why) = load(p) {
        if !replace_broken {
            return Err(format!("not saving: {why}. Nothing was changed"));
        }
        set_aside(p)?;
    }
    let text = serde_json::to_string_pretty(value).map_err(|e| e.to_string())?;
    write_atomic(p, text.as_bytes())
}

/// Moves a broken file (and its broken backup) aside as `.broken-<seconds>`, so nothing is lost and a fresh file
/// can start. Returns where it went.
pub fn set_aside(p: &Path) -> Result<PathBuf, String> {
    let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let to = with_ext(p, &format!(".broken-{t}"));
    if p.exists() {
        fs::rename(p, &to).map_err(|e| format!("can't move {} aside: {e}", p.display()))?;
    }
    let b = backup_path(p);
    if b.exists() {
        let _ = fs::rename(&b, with_ext(&to, ".bak"));
    }
    Ok(to)
}

/// Numbered save slots in a folder: `slot-1.json`… with a small summary each for the load screen.
pub struct Slots {
    pub dir: PathBuf,
}

#[derive(Debug, Clone, PartialEq)]
pub struct SlotInfo {
    pub slot: u32,
    pub summary: Value,
    pub from_backup: bool,
    pub broken: Option<String>,
}

impl Slots {
    pub fn path(&self, slot: u32) -> PathBuf {
        self.dir.join(format!("slot-{slot}.json"))
    }

    /// Saves a slot: `{"summary": …, "data": …}`.
    pub fn save(&self, slot: u32, summary: Value, data: Value) -> Result<(), String> {
        save_whole(&self.path(slot), &serde_json::json!({ "summary": summary, "data": data }), false)
    }

    pub fn load(&self, slot: u32) -> Result<Value, String> {
        match load(&self.path(slot)) {
            Loaded::Good(v) | Loaded::FromBackup(v) => Ok(v.get("data").cloned().unwrap_or(Value::Null)),
            Loaded::Missing => Err(format!("slot {slot} is empty")),
            Loaded::Broken(why) => Err(why),
        }
    }

    pub fn list(&self) -> Vec<SlotInfo> {
        let mut out = Vec::new();
        let Ok(entries) = fs::read_dir(&self.dir) else { return out };
        for e in entries.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            let Some(n) = name.strip_prefix("slot-").and_then(|r| r.strip_suffix(".json")).and_then(|n| n.parse::<u32>().ok()) else { continue };
            let info = match load(&e.path()) {
                Loaded::Good(v) => SlotInfo { slot: n, summary: v["summary"].clone(), from_backup: false, broken: None },
                Loaded::FromBackup(v) => SlotInfo { slot: n, summary: v["summary"].clone(), from_backup: true, broken: None },
                Loaded::Broken(why) => SlotInfo { slot: n, summary: Value::Null, from_backup: false, broken: Some(why) },
                Loaded::Missing => continue,
            };
            out.push(info);
        }
        out.sort_by_key(|s| s.slot);
        out
    }
}
