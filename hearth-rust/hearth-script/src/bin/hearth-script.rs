//! `hearth-script door.hs [event [value…]]`: runs a script, then fires an event, printing what `say` writes.
//! `hearth-script --check door.hs`: only looks for mistakes.

use hearth_script::{Engine, Value};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (check, rest) = match args.first().map(String::as_str) {
        Some("--check") => (true, &args[1..]),
        _ => (false, &args[..]),
    };
    let Some(path) = rest.first() else {
        eprintln!("usage: hearth-script [--check] script.hs [event [value…]]");
        std::process::exit(2);
    };
    let src = match std::fs::read_to_string(path) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("can't open {path}: {e}");
            std::process::exit(2);
        }
    };
    let name = std::path::Path::new(path).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    if check {
        match hearth_script::parser::parse(&src) {
            Ok(_) => println!("{name}: no mistakes found"),
            Err(e) => {
                eprintln!("{}", e.in_script(&name));
                std::process::exit(1);
            }
        }
        return;
    }
    let mut engine = Engine::new(&name);
    let result = engine.load(&src);
    for line in engine.take_output() {
        println!("{line}");
    }
    if let Err(e) = result {
        eprintln!("{e}");
        std::process::exit(1);
    }
    if let Some(event) = rest.get(1) {
        let values: Vec<Value> = rest[2..]
            .iter()
            .map(|v| match v.parse::<f64>() {
                Ok(n) => Value::Num(n),
                Err(_) => Value::text(v),
            })
            .collect();
        let result = engine.fire(event, &values);
        for line in engine.take_output() {
            println!("{line}");
        }
        if let Err(e) = result {
            eprintln!("{e}");
            std::process::exit(1);
        }
    }
}
