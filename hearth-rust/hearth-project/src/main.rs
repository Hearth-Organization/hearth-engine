//! hearth-project check|gems|enable GEM|disable GEM [--project DIR] [--home ~/.o3de]

use std::path::PathBuf;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let flag = |k: &str| args.iter().position(|a| a == k).and_then(|i| args.get(i + 1)).map(PathBuf::from);
    let project = flag("--project").unwrap_or_else(|| PathBuf::from("."));
    let home = match hearth_project::Home::load(flag("--home")) {
        Ok(h) => h,
        Err(e) => fail(&e),
    };
    let words: Vec<&String> = args.iter().enumerate().filter(|(i, a)| !a.starts_with("--") && (*i == 0 || !args[i - 1].starts_with("--"))).map(|(_, a)| a).collect();
    let result = match (words.first().map(|s| s.as_str()), words.get(1)) {
        (Some("check"), _) => hearth_project::check(&home, &project).map(|p| if p.is_empty() { "Every gem it uses is there: it will open.".to_string() } else { p.join("\n") }),
        (Some("gems"), _) => hearth_project::gems_for(&home, &project).map(|g| g.values().map(|g| format!("{}  {}", g.name, g.path.display())).collect::<Vec<_>>().join("\n")),
        (Some("enable"), Some(g)) => hearth_project::enable(&home, &project, g),
        (Some("disable"), Some(g)) => hearth_project::disable(&home, &project, g),
        _ => fail("usage: hearth-project check|gems|enable GEM|disable GEM [--project DIR] [--home DIR]"),
    };
    match result {
        Ok(s) => println!("{s}"),
        Err(e) => fail(&e),
    }
}

fn fail(e: &str) -> ! {
    eprintln!("{e}");
    std::process::exit(1)
}
