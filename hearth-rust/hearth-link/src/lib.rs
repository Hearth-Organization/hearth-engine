//! Hearth Link: the connection between Hearth Engine's Rust parts and the editor.
//!
//! The editor runs Hearth's bridge (the project's `Editor/Scripts/claude_bridge.py`), which listens on
//! 127.0.0.1 only, needs the session's secret token, and does each request on the editor's main thread.
//! Its port and token are in `<project>/user/ClaudeBridge/session.json`. Hearth Link reads that file,
//! sends requests over the socket (or, when a socket can't be opened, as files in the bridge's inbox),
//! and turns them into plain calls: [`Editor::level`], [`Editor::open_level`], [`Editor::play`],
//! [`Editor::selection`], [`Editor::place`] and the rest.
//!
//! The Hearth Engine agent (the helper that builds the game and starts the editor) is reached the same
//! way through [`Agent`], over its localhost API.
//!
//! Every editor call is one of a fixed list. Values travel as JSON, never pasted into code, so a level
//! or entity name can't change what runs.

mod agent;
mod bridge;
mod editor;
mod error;
mod session;

pub use agent::Agent;
pub use bridge::{Bridge, Reply, Transport};
pub use editor::{Editor, EntityInfo, LevelInfo};
pub use error::{Error, Result};
pub use session::{project_from, Session};
