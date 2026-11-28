//! `hearth-import model.glb [--json]`: what's wrong with a model, in plain words. Exit 1 when it can't be used.

use hearth_import::{check, Level, Limits};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(path) = args.iter().find(|a| !a.starts_with("--")) else {
        eprintln!("usage: hearth-import model.glb|model.obj [--json]");
        std::process::exit(2);
    };
    let mesh = match hearth_detail::load::load(std::path::Path::new(path)) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("{path}: {e}");
            std::process::exit(1);
        }
    };
    let r = check(&mesh, &Limits::default());
    if args.iter().any(|a| a == "--json") {
        println!("{}", r.to_json());
    } else {
        print!("{path}: {}", r.to_text());
    }
    if r.worst() == Level::Stop {
        std::process::exit(1);
    }
}
