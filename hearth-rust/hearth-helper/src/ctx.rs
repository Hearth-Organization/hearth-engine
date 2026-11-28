//! Where everything is for each game, and what the helper is doing for it.
//!
//! One helper serves every game in the projects folder at once (the owner, Nov 28: one editor, many games, "need
//! to just work"). Each request names its game (or gets the current one); each game has its own paths, logs,
//! builds and state. A thread knows which game it works for, and threads it starts carry that game on.

use std::cell::Cell;
use std::path::{Path, PathBuf};
use std::process::Child;
use std::sync::{Mutex, MutexGuard};

use serde_json::{json, Value};

/// The Python helper's version this one matches, operation for operation (hearth_agent.py VERSION). The owner
/// picks every version number; this isn't a new one.
pub const VERSION: &str = "1.10";
pub const HTTP_PORT: u16 = 47830;
pub const ENGINE_REPO: &str = "https://github.com/Dragongamer9/hearth-engine";
pub const ENGINE_BRANCH: &str = "hearth/main";

pub struct Paths {
    pub project: PathBuf,
    /// project.json's project_name (NoclipProtocol): its launchers are <name>.GameLauncher and so on.
    pub name: String,
    pub home: PathBuf,
    pub inbox: PathBuf,
    pub outbox: PathBuf,
    pub token_file: PathBuf,
    pub heartbeat: PathBuf,
    pub launch_file: PathBuf,
    pub build_log: PathBuf,
    pub agent_log: PathBuf,
    pub lock_file: PathBuf,
    pub bridge: PathBuf,
    pub log_dir: PathBuf,
    pub launcher: PathBuf,
    pub engine_dir: PathBuf,
    pub engine_log: PathBuf,
    pub stats_file: PathBuf,
    pub projects_root: PathBuf,
    pub shots: PathBuf,
    pub project_json: PathBuf,
    pub hub_dir: PathBuf,
    pub level0_prefab: PathBuf,
    pub levelgen_tool: PathBuf,
    pub levelgen_sources: Vec<PathBuf>,
    pub project_settings_file: PathBuf,
    pub coop_log: PathBuf,
    pub coop_log2: PathBuf,
}

pub fn user_home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/"))
}

pub fn config_dir() -> PathBuf {
    match std::env::var_os("XDG_CONFIG_HOME") {
        Some(p) if !p.is_empty() => PathBuf::from(p),
        _ => user_home().join(".config"),
    }
}

impl Paths {
    pub fn for_project(project: &Path) -> Paths {
        let project = project.to_path_buf();
        let name = std::fs::read_to_string(project.join("project.json"))
            .ok()
            .and_then(|t| serde_json::from_str::<Value>(&t).ok())
            .and_then(|v| v.get("project_name").and_then(Value::as_str).map(str::to_string))
            .unwrap_or_else(|| "NoclipProtocol".into());
        let home = project.join("user").join("HearthAgent");
        let bridge = project.join("user").join("ClaudeBridge");
        Paths {
            inbox: home.join("inbox"),
            outbox: home.join("outbox"),
            token_file: home.join("token"),
            heartbeat: home.join("heartbeat.json"),
            launch_file: home.join("editor_launch.json"),
            build_log: home.join("build.log"),
            agent_log: home.join("agent.log"),
            lock_file: home.join("agent.lock"),
            log_dir: project.join("user").join("log"),
            launcher: project.join(format!("build/linux/bin/profile/{name}.GameLauncher")),
            engine_dir: user_home().join("O3DE/Engines/hearth-engine"),
            engine_log: home.join("engine_build.log"),
            stats_file: home.join("stats.json"),
            projects_root: project.parent().map(Path::to_path_buf).unwrap_or_else(|| project.clone()),
            shots: bridge.join("shots"),
            project_json: project.join("project.json"),
            hub_dir: project.join("Tools/HearthEngine/hub"),
            level0_prefab: project.join("Levels/Level0/Level0.prefab"),
            levelgen_tool: home.join("level0gen"),
            levelgen_sources: [
                "Tools/Level0/level0gen.cpp",
                "Gem/Source/Level/Level0Json.cpp",
                "Gem/Source/Level/Level0Json.h",
                "Gem/Source/Level/Level0Layout.cpp",
                "Gem/Source/Level/Level0Layout.h",
            ]
            .iter()
            .map(|p| project.join(p))
            .collect(),
            project_settings_file: project.join("Registry/hearth_project.setreg"),
            coop_log: home.join("coop_host.log"),
            coop_log2: home.join("coop_window2.log"),
            bridge,
            home,
            name,
            project,
        }
    }

    /// The game's build targets: its launchers and the editor.
    pub fn build_targets(&self) -> Vec<String> {
        let n = &self.name;
        vec![
            format!("{n}.GameLauncher"),
            format!("{n}.HeadlessServerLauncher"),
            format!("{n}.ServerLauncher"),
            format!("{n}.UnifiedLauncher"),
            "Editor".into(),
        ]
    }
}

/// What the helper is doing for one game: its long jobs and the programs it started.
pub struct State {
    pub build: Value,
    pub engine: Value,
    pub cloud: Value,
    pub package: Value,
    pub game: Option<Child>,
    pub coop_host: Option<Child>,
    pub coop_window2: Option<Child>,
    pub coop_port: u16,
    pub coop_started: Option<f64>,
    pub coop_settings: Value,
    pub paused_assets: Vec<i32>,
    pub seen_editor: Option<i32>,
}

impl State {
    fn new() -> State {
        State {
            build: json!({"running": false, "started": null, "finished": null, "exit_code": null}),
            engine: json!({"running": false, "step": null, "started": null, "finished": null, "exit_code": null}),
            cloud: json!({"running": false, "step": null, "run_id": null, "error": null, "finished": null}),
            package: json!({"running": false, "step": null, "error": null, "finished": null, "result": null}),
            game: None,
            coop_host: None,
            coop_window2: None,
            coop_port: 33450,
            coop_started: None,
            coop_settings: json!({}),
            paused_assets: Vec::new(),
            seen_editor: None,
        }
    }
}

pub struct Project {
    pub paths: Paths,
    pub state: Mutex<State>,
}

/// Every game the helper has worked on; kept for the helper's whole life (a handful at most).
static PROJECTS: Mutex<Vec<&'static Project>> = Mutex::new(Vec::new());
/// The game a request without one goes to (the Hub's "current" game).
static DEFAULT: Mutex<Option<&'static Project>> = Mutex::new(None);

thread_local! {
    static CURRENT: Cell<Option<&'static Project>> = const { Cell::new(None) };
}

fn canonical(path: &Path) -> PathBuf {
    std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf())
}

/// The game in this folder, made ready the first time it's asked for.
pub fn open(path: &Path) -> &'static Project {
    let path = canonical(path);
    let mut all = PROJECTS.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(p) = all.iter().find(|p| p.paths.project == path) {
        return p;
    }
    let project: &'static Project = Box::leak(Box::new(Project { paths: Paths::for_project(&path), state: Mutex::new(State::new()) }));
    let _ = std::fs::create_dir_all(&project.paths.inbox);
    let _ = std::fs::create_dir_all(&project.paths.outbox);
    all.push(project);
    project
}

pub fn all() -> Vec<&'static Project> {
    PROJECTS.lock().unwrap_or_else(|e| e.into_inner()).clone()
}

pub fn set_default(project: &'static Project) {
    *DEFAULT.lock().unwrap_or_else(|e| e.into_inner()) = Some(project);
}

pub fn default_project() -> &'static Project {
    let current = *DEFAULT.lock().unwrap_or_else(|e| e.into_inner());
    current.expect("the helper sets its game before anything runs")
}

/// Makes this thread work for `project` (requests, and the jobs they start).
pub fn enter(project: &'static Project) {
    CURRENT.with(|c| c.set(Some(project)));
}

pub fn current() -> &'static Project {
    CURRENT.with(|c| c.get()).unwrap_or_else(default_project)
}

/// The paths of the game this thread works for.
pub fn p() -> &'static Paths {
    &current().paths
}

/// This game's state, never poisoned for good: a panicked operation mustn't stop every later one.
pub fn state() -> MutexGuard<'static, State> {
    current().state.lock().unwrap_or_else(|e| e.into_inner())
}

/// A background job for the same game as the thread that starts it.
pub fn spawn<F: FnOnce() + Send + 'static>(f: F) -> std::thread::JoinHandle<()> {
    let project = current();
    std::thread::spawn(move || {
        enter(project);
        f()
    })
}

/// A game folder a request may name: one in the projects folder, with a project.json.
pub fn known_game(path: &str) -> Option<&'static Project> {
    let given = if let Some(rest) = path.strip_prefix("~/") { user_home().join(rest) } else { PathBuf::from(path) };
    let candidate = canonical(&given);
    let root = canonical(&default_project().paths.projects_root);
    (candidate.parent() == Some(root.as_path()) && candidate.join("project.json").is_file()).then(|| open(&candidate))
}

/// Sets one field of a job's state (`build`, `engine`, `cloud`, `package`).
pub fn set(which: &str, key: &str, value: Value) {
    let mut s = state();
    let slot = match which {
        "build" => &mut s.build,
        "engine" => &mut s.engine,
        "cloud" => &mut s.cloud,
        _ => &mut s.package,
    };
    if let Some(m) = slot.as_object_mut() {
        m.insert(key.into(), value);
    }
}

pub fn get(which: &str) -> Value {
    let s = state();
    match which {
        "build" => s.build.clone(),
        "engine" => s.engine.clone(),
        "cloud" => s.cloud.clone(),
        _ => s.package.clone(),
    }
}

pub fn running(which: &str) -> bool {
    get(which).get("running").and_then(Value::as_bool).unwrap_or(false)
}

/// The engine is one for all games: is any game's engine work running?
pub fn engine_running_anywhere() -> bool {
    all().iter().any(|p| p.state.lock().map(|s| s.engine["running"] == true).unwrap_or(false))
}
