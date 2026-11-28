//! Every operation the helper does: a fixed list (it never runs anything else), by the same names and with the
//! same arguments and answers as the Python helper, so the Hub and Claude don't notice the change.

use serde_json::{json, Value};

use crate::util::setting_bool;

pub mod cloud;
pub mod coop;
pub mod core;
pub mod files;
pub mod forge;
pub mod hub;
pub mod levelgen;
pub mod voice;

type Op = fn(&Value) -> Value;

pub const OPS: &[(&str, Op)] = &[
    ("build", core::op_build),
    ("cloud_build", cloud::op_cloud_build),
    ("cloud_status", cloud::op_cloud_status),
    ("coop_start", coop::op_coop_start),
    ("coop_status", coop::op_coop_status),
    ("coop_stop", coop::op_coop_stop),
    ("crash_report", core::op_crash_report),
    ("dismiss_crash", hub::op_dismiss_crash),
    ("editor_start", core::op_editor_start),
    ("editor_stop", core::op_editor_stop),
    ("engine", hub::op_engine),
    ("engine_check", files::op_engine_check),
    ("engine_python", files::op_engine_python),
    ("forge_import", forge::op_forge_import),
    ("forge_made", forge::op_forge_made),
    ("forge_make", forge::op_forge_make),
    ("forge_models", forge::op_forge_models),
    ("forge_service", forge::op_forge_service),
    ("forge_status", forge::op_forge_status),
    ("game_start", core::op_game_start),
    ("game_stop", core::op_game_stop),
    ("hub_info", hub::op_hub_info),
    ("levelgen_apply", levelgen::op_levelgen_apply),
    ("levelgen_area", levelgen::op_levelgen_area),
    ("levelgen_get", levelgen::op_levelgen_get),
    ("levelgen_play", levelgen::op_levelgen_play),
    ("levelgen_tests", levelgen::op_levelgen_tests),
    ("list_projects", hub::op_list_projects),
    ("logs", core::op_logs),
    ("new_project", hub::op_new_project),
    ("package", cloud::op_package),
    ("package_checks", cloud::op_package_checks),
    ("package_status", cloud::op_package_status),
    ("parts", hub::op_parts),
    ("probe", core::op_probe),
    ("project_copy", files::op_project_copy),
    ("project_parts", hub::op_project_parts),
    ("set_part", hub::op_set_part),
    ("set_parts", hub::op_set_parts),
    ("set_project_part", hub::op_set_project_part),
    ("status", core::op_status),
    ("switch_project", hub::op_switch_project),
    ("unpack", files::op_unpack),
    ("voice_design", voice::op_voice_design),
    ("voice_lines", voice::op_voice_lines),
    ("voice_status", voice::op_voice_status),
];

pub fn find(op: &str) -> Option<Op> {
    OPS.iter().find(|(name, _)| *name == op).map(|(_, f)| *f)
}

pub fn names() -> Vec<&'static str> {
    OPS.iter().map(|(n, _)| *n).collect()
}

/// Runs an operation, never letting a panic take the helper down (the answer says what went wrong).
pub fn call(op: Op, args: &Value) -> Value {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| op(args))) {
        Ok(v) => v,
        Err(e) => {
            let why = e.downcast_ref::<String>().cloned().or_else(|| e.downcast_ref::<&str>().map(|s| s.to_string())).unwrap_or_else(|| "it stopped unexpectedly".into());
            json!({"ok": false, "error": format!("the helper's {why}")})
        }
    }
}

// Hearth Settings › Claude switches, applied to requests from Claude (the file inbox its hearth_engine tool uses).
const GATES: [(&str, &str, &[&str]); 3] = [
    ("readLogs", "Can read logs", &["logs", "crash_report"]),
    ("buildAndPlay", "Can build and play", &["build", "cloud_build", "package", "game_start", "game_stop", "editor_start", "editor_stop", "coop_start", "coop_stop", "levelgen_play", "engine"]),
    ("editLevels", "Can edit levels", &["levelgen_apply", "levelgen_area", "set_project_part", "set_part", "set_parts"]),
];

pub fn claude_refusal(op: &str, args: &Value) -> Option<Value> {
    let action = args.get("action").and_then(Value::as_str).unwrap_or("status");
    if op == "engine" && (action == "status" || action == "stop") {
        return None;
    }
    for (key, name, ops) in GATES {
        if ops.contains(&op) && !setting_bool("claude", key, true) {
            return Some(json!({"ok": false, "error": format!("Hearth Settings › Claude: '{name}' is off")}));
        }
    }
    let big = (op == "engine" && ["build", "update", "setup"].contains(&action))
        || (op == "build" && crate::util::truthy(args.get("reconfigure")))
        || op == "cloud_build"
        || op == "package";
    if big && setting_bool("claude", "askBigBuilds", true) && !crate::util::truthy(args.get("owner_ok")) {
        return Some(json!({"ok": false, "needs_owner": true,
            "error": "Hearth Settings › Claude: 'Ask before big builds' is on. Ask the owner in chat, then send the same request with owner_ok: true"}));
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_python_operation_is_here_once() {
        let python = [
            "package_checks", "package", "package_status", "project_parts", "set_project_part", "cloud_build", "cloud_status", "engine",
            "new_project", "hub_info", "set_part", "list_projects", "switch_project", "dismiss_crash", "coop_start", "coop_status", "coop_stop",
            "probe", "parts", "set_parts", "status", "editor_start", "editor_stop", "build", "game_start", "game_stop", "crash_report", "logs",
            "levelgen_get", "levelgen_area", "levelgen_tests", "levelgen_apply", "levelgen_play", "forge_status", "forge_service",
            "forge_models", "forge_make", "forge_made", "forge_import", "voice_status", "voice_design", "voice_lines", "engine_check",
            "engine_python", "project_copy", "unpack",
        ];
        assert_eq!(python.len(), 46);
        for op in python {
            assert!(find(op).is_some(), "missing {op}");
        }
        let mut n = names();
        n.sort();
        n.dedup();
        assert_eq!(n.len(), OPS.len(), "no operation twice");
        assert_eq!(OPS.len(), 46);
    }
}
