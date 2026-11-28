use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use serde::Deserialize;

use crate::error::{Error, Result};

/// The bridge's session, from `<project>/user/ClaudeBridge/session.json` (written by the editor, readable only
/// by this user).
#[derive(Debug, Clone, Deserialize, PartialEq)]
pub struct Session {
    pub port: u16,
    pub token: String,
    #[serde(default)]
    pub pid: Option<u32>,
    #[serde(default)]
    pub started: Option<f64>,
}

impl Session {
    pub fn folder(project: &Path) -> PathBuf {
        project.join("user").join("ClaudeBridge")
    }

    /// Reads the session the editor wrote when its bridge started.
    pub fn read(project: &Path) -> Result<Session> {
        let path = Self::folder(project).join("session.json");
        let text = std::fs::read_to_string(&path)
            .map_err(|e| Error::EditorNotRunning(format!("no {}: {e}", path.display())))?;
        serde_json::from_str(&text).map_err(|e| Error::BadAnswer(format!("{}: {e}", path.display())))
    }

    /// True when the bridge has written its heartbeat (every 2 seconds while the editor runs) within `max_age`.
    pub fn alive(project: &Path, max_age: Duration) -> bool {
        let beat = Self::folder(project).join("heartbeat.json");
        std::fs::metadata(beat)
            .and_then(|m| m.modified())
            .ok()
            .and_then(|t| SystemTime::now().duration_since(t).ok())
            .map(|age| age <= max_age)
            .unwrap_or(false)
    }
}

/// The project to work on: the given folder, else `HEARTH_PROJECT`, else the nearest folder up from `start`
/// that holds a `project.json`.
pub fn project_from(given: Option<&Path>, start: &Path) -> Option<PathBuf> {
    if let Some(p) = given {
        return Some(p.to_path_buf());
    }
    if let Some(p) = std::env::var_os("HEARTH_PROJECT") {
        if !p.is_empty() {
            return Some(PathBuf::from(p));
        }
    }
    let mut dir = Some(start);
    while let Some(d) = dir {
        if d.join("project.json").is_file() {
            return Some(d.to_path_buf());
        }
        dir = d.parent();
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_editors_session_file() {
        let dir = std::env::temp_dir().join(format!("hearth-link-session-{}", std::process::id()));
        let folder = Session::folder(&dir);
        std::fs::create_dir_all(&folder).unwrap();
        std::fs::write(folder.join("session.json"), r#"{"port": 47821, "token": "abc", "pid": 12, "started": 1.5}"#).unwrap();
        let s = Session::read(&dir).unwrap();
        assert_eq!(s.port, 47821);
        assert_eq!(s.token, "abc");
        assert_eq!(s.pid, Some(12));
        std::fs::write(folder.join("heartbeat.json"), "{}").unwrap();
        assert!(Session::alive(&dir, Duration::from_secs(10)));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn no_session_means_the_editor_isnt_running() {
        let dir = std::env::temp_dir().join(format!("hearth-link-none-{}", std::process::id()));
        assert!(matches!(Session::read(&dir), Err(Error::EditorNotRunning(_))));
        assert!(!Session::alive(&dir, Duration::from_secs(10)));
    }

    #[test]
    fn finds_the_project_above_the_current_folder() {
        let dir = std::env::temp_dir().join(format!("hearth-link-proj-{}", std::process::id()));
        let deep = dir.join("Levels").join("Level0");
        std::fs::create_dir_all(&deep).unwrap();
        std::fs::write(dir.join("project.json"), "{}").unwrap();
        // HEARTH_PROJECT may be set where the tests run; a given folder always wins.
        assert_eq!(project_from(Some(&dir), &deep), Some(dir.clone()));
        if std::env::var_os("HEARTH_PROJECT").is_none() {
            assert_eq!(project_from(None, &deep), Some(dir.clone()));
        }
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
