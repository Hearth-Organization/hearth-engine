use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};

use crate::bridge::Bridge;
use crate::error::{Error, Result};

/// The level that's open in the editor.
#[derive(Debug, Clone, Deserialize, Serialize, PartialEq)]
pub struct LevelInfo {
    pub name: String,
    pub playing: bool,
}

/// An entity in the level: its name and the editor's id for it (like "[1234567890]").
#[derive(Debug, Clone, Deserialize, Serialize, PartialEq)]
pub struct EntityInfo {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub position: Option<[f64; 3]>,
}

/// The line every call prints its result on, so it can be told apart from anything else the editor printed.
const MARK: &str = "HEARTH_LINK ";

/// Every call's Python starts the same way: the editor's modules, the values as JSON (`_a`), and a way to hand
/// the result back (`_out`). The values are never pasted into the code.
fn program(body: &str, args: &Value) -> String {
    let args_text = args.to_string();
    // A JSON string is also a valid Python string literal, so the values arrive exactly as sent.
    let literal = serde_json::to_string(&args_text).expect("a string always turns into JSON");
    format!(
        "import json as _j\n\
         import azlmbr.bus as bus\n\
         import azlmbr.editor as editor\n\
         import azlmbr.entity as entity\n\
         import azlmbr.components as components\n\
         import azlmbr.math as azmath\n\
         import azlmbr.legacy.general as general\n\
         _a = _j.loads({literal})\n\
         def _out(value):\n\
         \x20   print({mark:?} + _j.dumps(value))\n\
         {body}\n",
        mark = MARK
    )
}

/// Picks the result line out of what the editor printed.
fn result_of(output: &str) -> Result<Value> {
    let line = output
        .lines()
        .rev()
        .find_map(|l| l.strip_prefix(MARK))
        .ok_or_else(|| Error::BadAnswer(format!("no result in the editor's answer: {}", output.trim())))?;
    Ok(serde_json::from_str(line.trim())?)
}

// The calls' Python. Each is fixed; only `_a` changes.
const LEVEL: &str = "_out({'name': general.get_current_level_name() or '', 'playing': bool(general.is_in_game_mode())})";
const OPEN_LEVEL: &str = "general.open_level_no_prompt(_a['name'])\n_out({'name': general.get_current_level_name() or ''})";
const SAVE_LEVEL: &str = "general.save_level()\n_out(True)";
const PLAY: &str = "if not general.is_in_game_mode():\n    general.enter_game_mode()\n_out(True)";
const STOP: &str = "if general.is_in_game_mode():\n    general.exit_game_mode()\n_out(True)";
const OPEN_TOOL: &str = "general.open_pane(_a['name'])\n_out(bool(general.is_pane_visible(_a['name'])))";
const ENTITY_INFO: &str = "\
def _info(e):
    p = components.TransformBus(bus.Event, 'GetWorldTranslation', e)
    pos = [p.x, p.y, p.z] if p is not None else None
    return {'id': e.ToString(), 'name': editor.EditorEntityInfoRequestBus(bus.Event, 'GetName', e) or '', 'position': pos}
";
const SELECTION: &str = "_out([_info(e) for e in (editor.ToolsApplicationRequestBus(bus.Broadcast, 'GetSelectedEntities') or [])])";
const FIND: &str = "\
_f = entity.SearchFilter()
_f.names = [_a['name']]
_out([_info(e) for e in (entity.SearchBus(bus.Broadcast, 'SearchEntities', _f) or [])])";
const SELECT: &str = "\
_f = entity.SearchFilter()
_f.names = [_a['name']]
_found = list(entity.SearchBus(bus.Broadcast, 'SearchEntities', _f) or [])
editor.ToolsApplicationRequestBus(bus.Broadcast, 'SetSelectedEntities', _found)
_out(len(_found))";
const PLACE: &str = "\
_e = editor.ToolsApplicationRequestBus(bus.Broadcast, 'CreateNewEntity', entity.EntityId())
editor.EditorEntityAPIBus(bus.Event, 'SetName', _e, _a['name'])
components.TransformBus(bus.Event, 'SetWorldTranslation', _e, azmath.Vector3(float(_a['x']), float(_a['y']), float(_a['z'])))
_out(_info(_e))";
const MOVE: &str = "\
_f = entity.SearchFilter()
_f.names = [_a['name']]
_found = list(entity.SearchBus(bus.Broadcast, 'SearchEntities', _f) or [])
for _e in _found:
    components.TransformBus(bus.Event, 'SetWorldTranslation', _e, azmath.Vector3(float(_a['x']), float(_a['y']), float(_a['z'])))
_out(len(_found))";

/// The editor, through its bridge: the calls Hearth's Rust parts make.
#[derive(Debug, Clone)]
pub struct Editor {
    bridge: Bridge,
}

impl Editor {
    pub fn new(bridge: Bridge) -> Editor {
        Editor { bridge }
    }

    pub fn bridge(&self) -> &Bridge {
        &self.bridge
    }

    fn call(&self, body: &str, args: Value) -> Result<Value> {
        let mut fields = Map::new();
        fields.insert("code".into(), json!(program(body, &args)));
        let reply = self.bridge.request("python", fields)?.into_result()?;
        result_of(reply.output())
    }

    fn call_with_info(&self, body: &str, args: Value) -> Result<Value> {
        self.call(&format!("{ENTITY_INFO}{body}"), args)
    }

    /// The open level and whether the game is playing in the editor.
    pub fn level(&self) -> Result<LevelInfo> {
        Ok(serde_json::from_value(self.call(LEVEL, json!({}))?)?)
    }

    /// Opens a level by name (like "Level0"), without asking to save the one that's open.
    pub fn open_level(&self, name: &str) -> Result<String> {
        let v = self.call(OPEN_LEVEL, json!({ "name": name }))?;
        Ok(v.get("name").and_then(Value::as_str).unwrap_or_default().to_string())
    }

    pub fn save_level(&self) -> Result<()> {
        self.call(SAVE_LEVEL, json!({})).map(|_| ())
    }

    /// Plays the game in the editor's World tab.
    pub fn play(&self) -> Result<()> {
        self.call(PLAY, json!({})).map(|_| ())
    }

    pub fn stop(&self) -> Result<()> {
        self.call(STOP, json!({})).map(|_| ())
    }

    /// Opens one of the editor's tools by name ("Voice Studio"); Hearth's tools open as a tab in the one window.
    pub fn open_tool(&self, name: &str) -> Result<bool> {
        Ok(self.call(OPEN_TOOL, json!({ "name": name }))?.as_bool().unwrap_or(false))
    }

    /// What's selected in the editor.
    pub fn selection(&self) -> Result<Vec<EntityInfo>> {
        Ok(serde_json::from_value(self.call_with_info(SELECTION, json!({}))?)?)
    }

    /// The entities with this name.
    pub fn find(&self, name: &str) -> Result<Vec<EntityInfo>> {
        Ok(serde_json::from_value(self.call_with_info(FIND, json!({ "name": name }))?)?)
    }

    /// Selects the entities with this name; returns how many there were.
    pub fn select(&self, name: &str) -> Result<usize> {
        Ok(self.call(SELECT, json!({ "name": name }))?.as_u64().unwrap_or(0) as usize)
    }

    /// Makes a new, empty entity with this name at a place in the level (metres).
    pub fn place(&self, name: &str, at: [f64; 3]) -> Result<EntityInfo> {
        Ok(serde_json::from_value(
            self.call_with_info(PLACE, json!({ "name": name, "x": at[0], "y": at[1], "z": at[2] }))?,
        )?)
    }

    /// Moves the entities with this name; returns how many moved.
    pub fn move_to(&self, name: &str, at: [f64; 3]) -> Result<usize> {
        Ok(self.call(MOVE, json!({ "name": name, "x": at[0], "y": at[1], "z": at[2] }))?.as_u64().unwrap_or(0) as usize)
    }

    /// Runs one editor console command (like "r_displayInfo 0").
    pub fn console(&self, command: &str) -> Result<String> {
        let mut fields = Map::new();
        fields.insert("command".into(), json!(command));
        Ok(self.bridge.request("console", fields)?.into_result()?.output().to_string())
    }

    /// Saves a picture of the 3D view to `path` (a full path ending .png).
    pub fn screenshot(&self, path: &std::path::Path) -> Result<String> {
        let mut fields = Map::new();
        fields.insert("path".into(), json!(path.to_string_lossy()));
        Ok(self.bridge.request("screenshot", fields)?.into_result()?.output().to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn values_arrive_as_sent_whatever_they_hold() {
        let tricky = "Door \"A\"\n'); import os; os.system('x') #\\ ünïcode";
        let code = program("_out(_a)", &json!({ "name": tricky }));
        // The value is inside one JSON string literal, never loose in the code.
        let line = code.lines().find(|l| l.starts_with("_a = _j.loads(")).unwrap();
        let literal = line.trim_start_matches("_a = _j.loads(").trim_end_matches(')');
        let inner: String = serde_json::from_str(literal).unwrap();
        let back: Value = serde_json::from_str(&inner).unwrap();
        assert_eq!(back["name"], tricky);
        assert_eq!(code.lines().filter(|l| l.contains("os.system")).count(), 1, "only inside the literal");
    }

    #[test]
    fn finds_the_result_among_other_prints() {
        let out = "Loading...\nwarning: something\nHEARTH_LINK {\"name\": \"Level0\", \"playing\": false}\n";
        let v = result_of(out).unwrap();
        let level: LevelInfo = serde_json::from_value(v).unwrap();
        assert_eq!(level, LevelInfo { name: "Level0".into(), playing: false });
        assert!(result_of("no result here").is_err());
    }

    #[test]
    fn every_call_hands_its_result_back() {
        for body in [LEVEL, OPEN_LEVEL, SAVE_LEVEL, PLAY, STOP, OPEN_TOOL, SELECTION, FIND, SELECT, PLACE, MOVE] {
            assert!(body.contains("_out("), "{body}");
            // The values only ever come from _a.
            assert!(!body.contains("%s") && !body.contains(".format("), "{body}");
        }
    }
}
