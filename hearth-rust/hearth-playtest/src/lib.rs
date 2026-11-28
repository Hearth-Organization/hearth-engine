//! Playtest reports. A game writes one JSON line per thing that happens while someone plays:
//!
//! ```text
//! {"t": 12.5, "kind": "position", "x": 3, "y": 4, "z": 0, "level": "Level 0"}
//! {"t": 40.1, "kind": "died", "x": 8, "y": 2, "z": 0, "by": "Smiler"}
//! {"t": 41.0, "kind": "frame", "ms": 48}
//! {"t": 90.0, "kind": "quit"}
//! ```
//!
//! and this turns a folder of them into what a game maker wants to know: how long people played, where they died
//! and to what, where they got stuck (stayed in one small place a long time), where the game was slow, which
//! levels took how long, and where they gave up.

use std::collections::{BTreeMap, HashMap};

use serde_json::{json, Value};

#[derive(Clone, Debug, PartialEq)]
pub struct Event {
    pub t: f64,
    pub kind: String,
    pub pos: Option<[f64; 3]>,
    pub data: Value,
}

pub fn parse(text: &str) -> (Vec<Event>, usize) {
    let mut out = Vec::new();
    let mut bad = 0;
    for line in text.lines().filter(|l| !l.trim().is_empty()) {
        let Ok(v) = serde_json::from_str::<Value>(line) else {
            bad += 1;
            continue;
        };
        let (Some(t), Some(kind)) = (v["t"].as_f64(), v["kind"].as_str()) else {
            bad += 1;
            continue;
        };
        let pos = match (v["x"].as_f64(), v["y"].as_f64()) {
            (Some(x), Some(y)) => Some([x, y, v["z"].as_f64().unwrap_or(0.0)]),
            _ => None,
        };
        out.push(Event { t, kind: kind.to_string(), pos, data: v.clone() });
    }
    out.sort_by(|a, b| a.t.total_cmp(&b.t));
    (out, bad)
}

#[derive(Clone, Debug)]
pub struct Settings {
    /// Metres per heat-map cell.
    pub cell: f64,
    /// Staying within this many metres for at least `stuck_seconds` counts as stuck.
    pub stuck_radius: f64,
    pub stuck_seconds: f64,
    /// Frames slower than this are "slow" (50 ms = under 20 frames a second).
    pub slow_ms: f64,
}

impl Default for Settings {
    fn default() -> Settings {
        Settings { cell: 8.0, stuck_radius: 3.0, stuck_seconds: 45.0, slow_ms: 50.0 }
    }
}

fn cell_of(p: [f64; 3], size: f64) -> (i64, i64) {
    ((p[0] / size).floor() as i64, (p[1] / size).floor() as i64)
}

/// One player's session.
pub fn session(events: &[Event], s: &Settings) -> Value {
    let length = events.last().map(|e| e.t).unwrap_or(0.0) - events.first().map(|e| e.t).unwrap_or(0.0);
    let mut deaths: Vec<Value> = Vec::new();
    let mut killers: BTreeMap<String, u32> = BTreeMap::new();
    let mut levels: BTreeMap<String, f64> = BTreeMap::new();
    let mut stuck: Vec<Value> = Vec::new();
    let mut frames = 0u64;
    let mut slow = 0u64;
    let mut slow_places: HashMap<(i64, i64), u64> = HashMap::new();
    let mut last_pos: Option<[f64; 3]> = None;
    let mut level: Option<(String, f64)> = None;
    let mut anchor: Option<([f64; 3], f64)> = None;
    let mut reported_stuck = false;
    let mut quit = None;

    for e in events {
        if let Some(p) = e.pos {
            last_pos = Some(p);
            match anchor {
                Some((a, since)) if ((p[0] - a[0]).powi(2) + (p[1] - a[1]).powi(2)).sqrt() <= s.stuck_radius => {
                    if !reported_stuck && e.t - since >= s.stuck_seconds {
                        stuck.push(json!({"x": a[0], "y": a[1], "seconds": (e.t - since).round(), "at": since}));
                        reported_stuck = true;
                    } else if reported_stuck {
                        if let Some(last) = stuck.last_mut() {
                            last["seconds"] = json!((e.t - since).round());
                        }
                    }
                }
                _ => {
                    anchor = Some((p, e.t));
                    reported_stuck = false;
                }
            }
        }
        if let Some(l) = e.data["level"].as_str() {
            match &level {
                Some((cur, _)) if cur == l => {}
                Some((cur, since)) => {
                    *levels.entry(cur.clone()).or_default() += e.t - since;
                    level = Some((l.to_string(), e.t));
                }
                None => level = Some((l.to_string(), e.t)),
            }
        }
        match e.kind.as_str() {
            "died" => {
                let by = e.data["by"].as_str().unwrap_or("something").to_string();
                *killers.entry(by.clone()).or_default() += 1;
                let p = e.pos.or(last_pos);
                deaths.push(json!({"t": e.t, "by": by, "x": p.map(|p| p[0]), "y": p.map(|p| p[1])}));
            }
            "frame" => {
                frames += 1;
                if e.data["ms"].as_f64().unwrap_or(0.0) > s.slow_ms {
                    slow += 1;
                    if let Some(p) = last_pos {
                        *slow_places.entry(cell_of(p, s.cell)).or_default() += 1;
                    }
                }
            }
            "quit" => quit = Some((e.t, last_pos)),
            _ => {}
        }
    }
    if let (Some((cur, since)), Some(end)) = (&level, events.last()) {
        *levels.entry(cur.clone()).or_default() += end.t - since;
    }
    let mut slow_list: Vec<((i64, i64), u64)> = slow_places.into_iter().collect();
    slow_list.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    json!({
        "seconds": length.round(),
        "deaths": deaths,
        "killed_by": killers,
        "levels": levels.iter().map(|(k, v)| (k.clone(), json!(v.round()))).collect::<serde_json::Map<_, _>>(),
        "stuck": stuck,
        "frames": frames,
        "slow_frames": slow,
        "slow_places": slow_list.iter().take(5).map(|((x, y), n)| json!({"x": *x as f64 * s.cell, "y": *y as f64 * s.cell, "frames": n})).collect::<Vec<_>>(),
        "quit": quit.map(|(t, p)| json!({"t": t, "x": p.map(|p| p[0]), "y": p.map(|p| p[1])})),
    })
}

/// Many sessions: totals and the places that came up most.
pub fn combine(sessions: &[Value], s: &Settings) -> Value {
    let n = sessions.len().max(1) as f64;
    let total: f64 = sessions.iter().map(|x| x["seconds"].as_f64().unwrap_or(0.0)).sum();
    let mut killers: BTreeMap<String, u64> = BTreeMap::new();
    let mut death_cells: HashMap<(i64, i64), u64> = HashMap::new();
    let mut quits = 0;
    for x in sessions {
        for (k, v) in x["killed_by"].as_object().into_iter().flatten() {
            *killers.entry(k.clone()).or_default() += v.as_u64().unwrap_or(0);
        }
        for d in x["deaths"].as_array().into_iter().flatten() {
            if let (Some(px), Some(py)) = (d["x"].as_f64(), d["y"].as_f64()) {
                *death_cells.entry(cell_of([px, py, 0.0], s.cell)).or_default() += 1;
            }
        }
        if !x["quit"].is_null() {
            quits += 1;
        }
    }
    let mut hot: Vec<((i64, i64), u64)> = death_cells.into_iter().collect();
    hot.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    json!({
        "sessions": sessions.len(),
        "average_minutes": (total / n / 60.0 * 10.0).round() / 10.0,
        "killed_by": killers,
        "deadliest_places": hot.iter().take(5).map(|((x, y), c)| json!({"x": *x as f64 * s.cell, "y": *y as f64 * s.cell, "deaths": c})).collect::<Vec<_>>(),
        "quit_early": quits,
    })
}

/// The report in plain words.
pub fn words(all: &Value) -> String {
    let mut s = format!("{} playtests, {} minutes each on average.\n", all["sessions"], all["average_minutes"]);
    if let Some(k) = all["killed_by"].as_object().filter(|k| !k.is_empty()) {
        let mut list: Vec<(&String, u64)> = k.iter().map(|(a, b)| (a, b.as_u64().unwrap_or(0))).collect();
        list.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(b.0)));
        s += &format!("Most deaths: {}.\n", list.iter().map(|(a, b)| format!("{a} ({b})")).collect::<Vec<_>>().join(", "));
    }
    for p in all["deadliest_places"].as_array().into_iter().flatten().take(3) {
        s += &format!("- {} deaths around x {}, y {}\n", p["deaths"], p["x"], p["y"]);
    }
    s
}
