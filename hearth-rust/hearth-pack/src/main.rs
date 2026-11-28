//! hearth-pack --export DIR --target hearthos|steam|test --name NAME --display "Name" --version V --out DIR
//!             [--icon icon.png] [--leave-out-editor] [--extra from=to ...]
//! hearth-pack verify FILE SHA256SUMS.txt

use std::path::PathBuf;

use hearth_pack::{make, verify, Package, Target};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.first().map(String::as_str) == Some("verify") {
        match (args.get(1), args.get(2)) {
            (Some(f), Some(s)) => match verify(&PathBuf::from(f), &PathBuf::from(s)) {
                Ok(()) => println!("{f}: matches"),
                Err(e) => {
                    eprintln!("{e}");
                    std::process::exit(1);
                }
            },
            _ => {
                eprintln!("usage: hearth-pack verify FILE SHA256SUMS.txt");
                std::process::exit(2);
            }
        }
        return;
    }
    let mut get = std::collections::HashMap::new();
    let mut extras = Vec::new();
    let mut leave_out = false;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--leave-out-editor" => leave_out = true,
            "--extra" => {
                if let Some((f, t)) = it.next().and_then(|v| v.split_once('=')) {
                    extras.push((PathBuf::from(f), t.to_string()));
                }
            }
            k if k.starts_with("--") => {
                get.insert(k.trim_start_matches("--").to_string(), it.next().cloned().unwrap_or_default());
            }
            _ => {}
        }
    }
    let need = |k: &str| -> String {
        get.get(k).cloned().unwrap_or_else(|| {
            eprintln!("--{k} is needed");
            std::process::exit(2)
        })
    };
    let target = match Target::parse(&need("target")) {
        Ok(t) => t,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(2)
        }
    };
    let p = Package {
        export: need("export").into(),
        target,
        name: need("name"),
        display: need("display"),
        version: need("version"),
        out: need("out").into(),
        icon: get.get("icon").map(PathBuf::from),
        leave_out_editor: leave_out,
        extras,
        appimagetool: None,
    };
    match make(&p) {
        Ok(files) => files.iter().for_each(|f| println!("PACKAGE {}", f.display())),
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    }
}
