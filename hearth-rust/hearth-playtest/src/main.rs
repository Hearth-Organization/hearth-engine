//! `hearth-playtest FOLDER [--json]`: a report from every .jsonl playtest recording in a folder.

use hearth_playtest::{combine, parse, session, words, Settings};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(dir) = args.iter().find(|a| !a.starts_with("--")) else {
        eprintln!("usage: hearth-playtest FOLDER [--json]");
        std::process::exit(2);
    };
    let s = Settings::default();
    let mut sessions = Vec::new();
    let mut files: Vec<_> = std::fs::read_dir(dir).map(|r| r.flatten().map(|e| e.path()).collect()).unwrap_or_default();
    files.sort();
    for f in files.iter().filter(|f| f.extension().map(|e| e == "jsonl").unwrap_or(false)) {
        let text = std::fs::read_to_string(f).unwrap_or_default();
        let (events, bad) = parse(&text);
        if bad > 0 {
            eprintln!("{}: {bad} lines couldn't be read", f.display());
        }
        sessions.push(session(&events, &s));
    }
    let all = combine(&sessions, &s);
    if args.iter().any(|a| a == "--json") {
        println!("{}", serde_json::json!({ "all": all, "sessions": sessions }));
    } else {
        print!("{}", words(&all));
    }
}
