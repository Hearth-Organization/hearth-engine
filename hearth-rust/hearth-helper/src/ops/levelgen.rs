//! The Level Generator (Hearth Engine Hub): Level 0's settings, previews and tests from the game's own generator.

use std::fs;
use std::time::Duration;

use serde_json::{json, Map, Value};

use crate::ctx::p;
use crate::ops::core::{bridge_alive, bridge_python, op_game_start};
use crate::util::*;

/// Hub name -> (field in the level file, label in the Editor, default, generator key).
pub const FIELDS: [(&str, &str, &str, Default, Option<&str>); 12] = [
    ("seed", "Seed", "Seed", Default::Int(1), Some("seed")),
    ("newSeedEachRun", "RandomSeedEachRun", "Random seed each run", Default::Bool(true), None),
    ("openness", "OpenFraction", "Openness", Default::Float(0.2), Some("openFraction")),
    ("roomChance", "RoomChance", "Open room chance", Default::Float(0.35), Some("roomChance")),
    ("pillarChance", "PillarChance", "Pillar chance", Default::Float(0.6), Some("pillarChance")),
    ("baseSize", "SpawnRoomRadius", "Forward base size", Default::Int(3), Some("spawnRoomRadius")),
    ("darkness", "DarkZoneShare", "Darkness", Default::Float(0.4), Some("darkZoneShare")),
    ("darkZoneSize", "DarkZoneScale", "Dark zone size", Default::Float(3.5), Some("darkZoneScale")),
    ("stains", "DecalChance", "Stains", Default::Float(0.3), Some("decalChance")),
    ("flicker", "EnableFlicker", "Flickering lights", Default::Bool(true), None),
    ("dying", "EnableDyingLights", "Dying lights", Default::Bool(true), None),
    ("roomShifting", "EnableRoomShifting", "Room shifting (soon)", Default::Bool(false), None),
];

#[derive(Clone, Copy)]
pub enum Default {
    Int(i64),
    Float(f64),
    Bool(bool),
}

impl Default {
    fn value(self) -> Value {
        match self {
            Default::Int(i) => json!(i),
            Default::Float(f) => json!(f),
            Default::Bool(b) => json!(b),
        }
    }
}

/// The settings, cleaned: known names only, each its own type and in range. The seed is a 64-bit number (u64).
#[derive(Debug, Clone, PartialEq)]
pub struct Settings(pub Map<String, Value>);

fn as_f64(v: &Value) -> Option<f64> {
    match v {
        Value::Number(n) => n.as_f64(),
        Value::String(s) => s.trim().parse().ok(),
        Value::Bool(b) => Some(if *b { 1.0 } else { 0.0 }),
        _ => None,
    }
}

pub fn clean(values: &Value) -> Settings {
    let mut out = Map::new();
    for (name, _, _, default, _) in FIELDS {
        let given = values.get(name);
        let value = match (default, given) {
            (d, None) => d.value(),
            (Default::Bool(_), Some(v)) => json!(truthy(Some(v))),
            (Default::Int(d), Some(v)) if name == "seed" => {
                // Python: int(str(value).strip() or "1") % 2**64 (seeds arrive as text from the Hub).
                let s = match v {
                    Value::String(s) => s.trim().to_string(),
                    other => other.to_string(),
                };
                let s = if s.is_empty() { "1".to_string() } else { s };
                match s.parse::<i128>() {
                    Ok(n) => json!(n.rem_euclid(1i128 << 64) as u64),
                    Err(_) => json!(d),
                }
            }
            (Default::Int(d), Some(v)) => json!(as_f64(v).map(|f| f.round() as i64).unwrap_or(d)),
            (Default::Float(d), Some(v)) => json!(as_f64(v).unwrap_or(d)),
        };
        out.insert(name.to_string(), value);
    }
    let clamp = |out: &mut Map<String, Value>, k: &str, lo: f64, hi: f64| {
        let v = out.get(k).and_then(Value::as_f64).unwrap_or(lo);
        out.insert(k.into(), json!(v.clamp(lo, hi)));
    };
    for k in ["openness", "roomChance", "pillarChance", "darkness", "stains"] {
        clamp(&mut out, k, 0.0, 1.0);
    }
    clamp(&mut out, "darkZoneSize", 1.0, 8.0);
    let base = out.get("baseSize").and_then(Value::as_i64).unwrap_or(3).clamp(0, 5);
    out.insert("baseSize".into(), json!(base));
    out.insert("roomShifting".into(), json!(false)); // not built yet
    Settings(out)
}

/// A number the way Python prints it (3.0 stays "3.0", 1 stays "1"), so the generator gets the same text.
fn py(v: &Value) -> String {
    match v {
        Value::Bool(b) => if *b { "True".into() } else { "False".into() },
        Value::Number(n) if n.is_f64() => {
            let f = n.as_f64().unwrap_or(0.0);
            if f.fract() == 0.0 && f.abs() < 1e16 { format!("{f:.1}") } else { format!("{f}") }
        }
        Value::Number(n) => n.to_string(),
        other => other.to_string(),
    }
}

/// The generator's settings line: "seed=1;openFraction=0.2;...;enableFlicker=1;enableDying=1".
pub fn line(values: &Settings) -> String {
    let v = &values.0;
    let mut parts: Vec<String> = FIELDS.iter().filter_map(|(name, _, _, _, key)| key.map(|k| format!("{k}={}", py(&v[*name])))).collect();
    parts.push(format!("enableFlicker={}", if v["flicker"] == true { 1 } else { 0 }));
    parts.push(format!("enableDying={}", if v["dying"] == true { 1 } else { 0 }));
    parts.join(";")
}

/// Seeds go to the Hub as text (JavaScript numbers would round them).
pub fn seed_text(values: &Settings) -> Value {
    let mut out = values.0.clone();
    let s = out.get("seed").map(|v| v.to_string()).unwrap_or_else(|| "1".into());
    out.insert("seed".into(), json!(s));
    Value::Object(out)
}

fn level0_component(data: &mut Value) -> Option<&mut Map<String, Value>> {
    for entity in data.get_mut("Entities")?.as_object_mut()?.values_mut() {
        let Some(components) = entity.get_mut("Components").and_then(Value::as_object_mut) else { continue };
        for component in components.values_mut() {
            if let Some(t) = component.get_mut("m_template").and_then(Value::as_object_mut) {
                if t.get("$type").and_then(Value::as_str) == Some("EndlessLevelComponent") {
                    return Some(t);
                }
            }
        }
    }
    None
}

pub fn op_levelgen_get(_args: &Value) -> Value {
    let mut data = match fs::read_to_string(&p().level0_prefab).map_err(|e| e.to_string()).and_then(|t| serde_json::from_str::<Value>(&t).map_err(|e| e.to_string())) {
        Ok(d) => d,
        Err(e) => return err(format!("can't read the level: {e}")),
    };
    let component = level0_component(&mut data).cloned().unwrap_or_default();
    let mut values = Map::new();
    for (name, field, _, default, _) in FIELDS {
        values.insert(name.into(), component.get(field).cloned().unwrap_or_else(|| default.value()));
    }
    json!({"ok": true, "values": seed_text(&clean(&Value::Object(values))), "level": "Level 0", "name": "The Lobby"})
}

/// Builds the small level0gen tool from the game's own generator code when it is missing or out of date.
fn tool() -> Option<String> {
    let newest = p().levelgen_sources.iter().filter_map(|s| mtime(s)).fold(0.0, f64::max);
    if mtime(&p().levelgen_tool).map(|t| t >= newest).unwrap_or(false) {
        return None;
    }
    let Some(compiler) = which("clang++").or_else(|| which("g++")) else { return Some("no C++ compiler (clang++ or g++) found".into()) };
    let tmp = p().levelgen_tool.with_extension("new");
    let mut cmd: Vec<String> = vec!["nice".into(), "-n".into(), "10".into(), compiler.display().to_string(), "-std=c++17".into(), "-O2".into(), "-o".into(), tmp.display().to_string()];
    cmd.extend(p().levelgen_sources.iter().filter(|s| s.extension().map(|e| e == "cpp").unwrap_or(false)).map(|s| s.display().to_string()));
    let refs: Vec<&str> = cmd.iter().map(String::as_str).collect();
    let (_, errs, code) = run(&refs, None, Duration::from_secs(180));
    if code != 0 {
        return Some(format!("level0gen didn't compile: {}", last_chars(&errs, 1500)));
    }
    if let Err(e) = fs::rename(&tmp, &p().levelgen_tool) {
        return Some(format!("level0gen not saved: {e}"));
    }
    None
}

fn generator(argv: &[String], timeout: u64) -> Value {
    if let Some(problem) = tool() {
        return err(problem);
    }
    let mut cmd: Vec<&str> = vec![p().levelgen_tool.to_str().unwrap_or("")];
    cmd.extend(argv.iter().map(String::as_str));
    let (out, errs, code) = run(&cmd, None, Duration::from_secs(timeout));
    if code != 0 {
        let e = last_chars(&errs, 800);
        return err(if e.is_empty() { format!("exit code {code}") } else { e });
    }
    match serde_json::from_str::<Value>(&out) {
        Ok(d) => json!({"ok": true, "data": d}),
        Err(_) => err("level0gen gave bad output"),
    }
}

pub fn op_levelgen_area(args: &Value) -> Value {
    let values = clean(args.get("values").unwrap_or(&json!({})));
    let area = [int(args, "minX", -2), int(args, "minY", -2), int(args, "maxX", 1), int(args, "maxY", 1)];
    let mut argv = vec!["area".to_string(), line(&values)];
    argv.extend(area.iter().map(|n| n.to_string()));
    let mut result = generator(&argv, 60);
    if result["ok"] == true {
        result["values"] = seed_text(&values);
    }
    result
}

pub fn op_levelgen_tests(args: &Value) -> Value {
    let values = clean(args.get("values").unwrap_or(&json!({})));
    let seeds = int(args, "seeds", 50).clamp(1, 200);
    generator(&["tests".into(), line(&values), seeds.to_string()], 120)
}

/// Saves the settings into Level 0: through the open editor when it has the level, else in the level file.
pub fn op_levelgen_apply(args: &Value) -> Value {
    let values = clean(args.get("values").unwrap_or(&json!({})));
    if !editor_processes().is_empty() && bridge_alive(10.0) {
        let pairs: Map<String, Value> = FIELDS.iter().filter(|(n, ..)| *n != "roomShifting").map(|(n, _, label, _, _)| (label.to_string(), values.0[*n].clone())).collect();
        let pairs_json = serde_json::to_string(&serde_json::to_string(&Value::Object(pairs)).unwrap_or_default()).unwrap_or_default();
        let code = format!(
            "\nimport json as _j\nimport azlmbr.bus as b, azlmbr.editor as ed, azlmbr.entity as ent, azlmbr.legacy.general as g\n\
pairs = _j.loads({pairs_json})\n\
if g.get_current_level_name().lower() != 'level0':\n    print('NOT_LEVEL0')\nelse:\n\
    tid = ed.EditorComponentAPIBus(b.Broadcast, 'FindComponentTypeIdsByEntityType', ['Endless Level'], ent.EntityType().Game)\n\
    done = []\n\
    for e in ed.ToolsApplicationRequestBus(b.Broadcast, 'GetAllEntities') or []:\n\
        got = ed.EditorComponentAPIBus(b.Broadcast, 'GetComponentOfType', e, tid[0]) if tid else None\n\
        if got and got.IsSuccess():\n\
            comp = got.GetValue()\n\
            paths = ed.EditorComponentAPIBus(b.Broadcast, 'BuildComponentPropertyList', comp)\n\
            for label, value in pairs.items():\n\
                for p in paths:\n\
                    if p == label or p.endswith('|' + label):\n\
                        ok = ed.EditorComponentAPIBus(b.Broadcast, 'SetComponentProperty', comp, p, value).IsSuccess()\n\
                        done.append((label, ok))\n\
                        break\n\
    print('SET', done)\n\
    print('SAVED', g.save_level())\n"
        );
        let answer = bridge_python(&code, 60);
        let output = answer.as_ref().and_then(|a| a.get("output")).and_then(Value::as_str).unwrap_or("").to_string();
        if !output.contains("NOT_LEVEL0") && output.contains("SAVED True") {
            return json!({"ok": true, "via": "editor", "values": seed_text(&values), "detail": last_chars(&output, 600)});
        }
        if !editor_processes().is_empty() {
            return json!({"ok": false, "error": "the editor is open but couldn't apply the settings (is Level 0 open?)", "detail": last_chars(&output, 600)});
        }
    }
    let prefab = &p().level0_prefab;
    let mut data = match fs::read_to_string(prefab).map_err(|e| e.to_string()).and_then(|t| serde_json::from_str::<Value>(&t).map_err(|e| e.to_string())) {
        Ok(d) => d,
        Err(e) => return err(format!("can't read the level: {e}")),
    };
    let Some(component) = level0_component(&mut data) else { return err("Level 0 has no Endless Level component") };
    for (name, field, _, default, _) in FIELDS {
        let v = &values.0[name];
        let is_default = match default {
            Default::Float(d) => v.as_f64() == Some(d),
            other => *v == other.value(),
        };
        if is_default {
            component.remove(field);
        } else {
            component.insert(field.into(), v.clone());
        }
    }
    let backup = prefab.with_extension("prefab.bak");
    if let Err(e) = fs::copy(prefab, &backup) {
        return err(format!("couldn't back up the level: {e}"));
    }
    if let Err(e) = write_atomic(prefab, &(pretty(&data, 4) + "\n")) {
        return err(format!("couldn't save the level: {e}"));
    }
    json!({"ok": true, "via": "file", "values": seed_text(&values)})
}

/// Starts the game on Level 0 with exactly the settings on screen (not saved to the level).
pub fn op_levelgen_play(args: &Value) -> Value {
    let values = clean(args.get("values").unwrap_or(&json!({})));
    let extra = vec![format!("--regset=/NoclipProtocol/Level0/Override={}", line(&values))];
    op_game_start(&json!({"fullscreen": flag(args, "fullscreen", true), "level": "Level0",
                          "width": args.get("width").cloned().unwrap_or(json!(1600)), "height": args.get("height").cloned().unwrap_or(json!(900)), "extra": extra}))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cleans_like_the_python_helper() {
        let v = clean(&json!({"seed": "18446744073709551617", "openness": 3, "baseSize": 9.6, "darkZoneSize": 0.2, "flicker": 0, "roomShifting": true, "bogus": 1}));
        assert_eq!(v.0["seed"], json!(1u64), "seeds wrap at 2^64");
        assert_eq!(v.0["openness"], json!(1.0));
        assert_eq!(v.0["baseSize"], json!(5));
        assert_eq!(v.0["darkZoneSize"], json!(1.0));
        assert_eq!(v.0["flicker"], json!(false));
        assert_eq!(v.0["roomShifting"], json!(false));
        assert!(!v.0.contains_key("bogus"));
    }

    #[test]
    fn the_generator_line_matches_python() {
        let v = clean(&json!({}));
        assert_eq!(
            line(&v),
            "seed=1;openFraction=0.2;roomChance=0.35;pillarChance=0.6;spawnRoomRadius=3;darkZoneShare=0.4;darkZoneScale=3.5;decalChance=0.3;enableFlicker=1;enableDying=1"
        );
        let v = clean(&json!({"darkZoneSize": 3, "seed": 42}));
        assert!(line(&v).contains("darkZoneScale=3.0"), "{}", line(&v));
        assert_eq!(seed_text(&v)["seed"], json!("42"));
    }
}
