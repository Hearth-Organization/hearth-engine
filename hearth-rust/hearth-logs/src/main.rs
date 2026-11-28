//! `hearth-logs Editor.log [--json]`: what went wrong, in plain words.

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(path) = args.iter().find(|a| !a.starts_with("--")) else {
        eprintln!("usage: hearth-logs file.log [--json]");
        std::process::exit(2);
    };
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("can't open {path}: {e}");
            std::process::exit(2);
        }
    };
    let r = hearth_logs::read(&String::from_utf8_lossy(&bytes));
    if args.iter().any(|a| a == "--json") {
        println!("{}", r.to_json());
    } else {
        print!("{}", r.summary());
    }
}
