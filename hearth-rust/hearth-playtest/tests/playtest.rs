use hearth_playtest::{combine, parse, session, words, Settings};

fn recording(deaths_at: f64, quit: bool) -> String {
    let mut s = String::new();
    // Walks east for 60 s on Level 0, then stands still 60 s on Level 1 (stuck), dies, maybe quits.
    for i in 0..=60 {
        s += &format!("{{\"t\": {i}, \"kind\": \"position\", \"x\": {}, \"y\": 0, \"level\": \"Level 0\"}}\n", i * 2);
        s += &format!("{{\"t\": {i}.5, \"kind\": \"frame\", \"ms\": {}}}\n", if i > 50 { 80 } else { 16 });
    }
    for i in 61..=120 {
        s += &format!("{{\"t\": {i}, \"kind\": \"position\", \"x\": {deaths_at}, \"y\": 1, \"level\": \"Level 1\"}}\n");
    }
    s += &format!("{{\"t\": 121, \"kind\": \"died\", \"x\": {deaths_at}, \"y\": 1, \"by\": \"Smiler\"}}\n");
    if quit {
        s += "{\"t\": 122, \"kind\": \"quit\"}\n";
    }
    s += "not json\n";
    s
}

#[test]
fn a_session_finds_deaths_stuck_slow_and_level_times() {
    let s = Settings::default();
    let (events, bad) = parse(&recording(200.0, true));
    assert_eq!(bad, 1);
    let r = session(&events, &s);
    assert_eq!(r["seconds"], 122.0);
    assert_eq!(r["killed_by"]["Smiler"], 1);
    assert_eq!(r["deaths"][0]["x"], 200.0);
    assert_eq!(r["levels"]["Level 0"], 61.0);
    assert_eq!(r["levels"]["Level 1"], 61.0);
    let stuck = r["stuck"].as_array().unwrap();
    assert_eq!(stuck.len(), 1, "{stuck:?}");
    assert!(stuck[0]["seconds"].as_f64().unwrap() >= 59.0);
    assert_eq!(r["slow_frames"], 10);
    assert_eq!(r["slow_places"][0]["x"], 104.0, "slow near the end of the walk");
    assert!(!r["quit"].is_null());
}

#[test]
fn many_sessions_show_the_deadliest_places() {
    let s = Settings::default();
    let sessions: Vec<_> = [(200.0, true), (201.0, false), (90.0, false)].iter().map(|&(x, q)| session(&parse(&recording(x, q)).0, &s)).collect();
    let all = combine(&sessions, &s);
    assert_eq!(all["sessions"], 3);
    assert_eq!(all["killed_by"]["Smiler"], 3);
    assert_eq!(all["deadliest_places"][0]["deaths"], 2);
    assert_eq!(all["deadliest_places"][0]["x"], 200.0);
    assert_eq!(all["quit_early"], 1);
    let w = words(&all);
    assert!(w.contains("3 playtests") && w.contains("Smiler (3)"), "{w}");
}
