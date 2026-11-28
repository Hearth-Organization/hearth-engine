//! `hearth-link`: Hearth Link from the command line, for Claude, scripts and testing. Every answer is JSON.
//!
//!     hearth-link [--project PATH] [--files] COMMAND [ARGS]
//!
//! Commands: status, ping, level, open NAME, save, play, stop, tool NAME, selection, find NAME, select NAME,
//! place NAME X Y Z, move NAME X Y Z, console COMMAND..., screenshot PATH, agent OP [JSON].

use std::path::PathBuf;
use std::process::ExitCode;
use std::time::Duration;

use hearth_link::{project_from, Agent, Bridge, Editor, Error, Session, Transport};
use serde_json::{json, Value};

const HELP: &str = "hearth-link [--project PATH] [--files] COMMAND [ARGS]

  status                 is the editor open, and the agent running
  ping                   the editor answers
  level                  the open level, and whether it's playing
  open NAME              open a level (like Level0)
  save                   save the open level
  play | stop            play the game in the World tab, or stop
  tool NAME              open a tool (like \"Voice Studio\") as a tab
  selection              what's selected
  find NAME              the entities with this name
  select NAME            select the entities with this name
  place NAME X Y Z       a new entity at a place (metres)
  move NAME X Y Z        move the entities with this name
  console COMMAND...     one editor console command
  screenshot PATH        a picture of the 3D view (.png)
  agent OP [JSON]        one of the Hearth Engine agent's operations

  --project PATH         the game's folder (else HEARTH_PROJECT, else the folder you're in)
  --files                talk to the editor through files only (when a sandbox blocks connections)";

fn main() -> ExitCode {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let mut project: Option<PathBuf> = None;
    let mut transport = Transport::Auto;
    while let Some(first) = args.first().cloned() {
        match first.as_str() {
            "--project" if args.len() > 1 => {
                project = Some(PathBuf::from(&args[1]));
                args.drain(..2);
            }
            "--files" => {
                transport = Transport::Files;
                args.remove(0);
            }
            "-h" | "--help" | "help" => {
                println!("{HELP}");
                return ExitCode::SUCCESS;
            }
            _ => break,
        }
    }
    if args.is_empty() {
        eprintln!("{HELP}");
        return ExitCode::from(2);
    }
    let here = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    let Some(project) = project_from(project.as_deref(), &here) else {
        eprintln!("Which game? Run this in the game's folder, or give --project PATH.");
        return ExitCode::from(2);
    };
    match run(&project, transport, &args) {
        Ok(v) => {
            println!("{}", serde_json::to_string_pretty(&v).unwrap_or_default());
            ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("{e}");
            println!("{}", json!({ "ok": false, "error": e.to_string() }));
            ExitCode::FAILURE
        }
    }
}

fn number(s: Option<&String>) -> Result<f64, Error> {
    s.and_then(|s| s.parse().ok()).ok_or_else(|| Error::BadAnswer("X Y Z must be numbers".into()))
}

fn need<'a>(args: &'a [String], i: usize, what: &str) -> Result<&'a str, Error> {
    args.get(i).map(String::as_str).ok_or_else(|| Error::BadAnswer(format!("missing {what}")))
}

fn run(project: &std::path::Path, transport: Transport, args: &[String]) -> Result<Value, Error> {
    let command = args[0].as_str();
    if command == "status" {
        let editor = Session::alive(project, Duration::from_secs(10));
        let agent = Agent::connect(project).and_then(|a| a.timeout(Duration::from_secs(10)).call("status", &json!({})));
        return Ok(json!({
            "project": project.display().to_string(),
            "editor": editor,
            "agent": agent.as_ref().ok().cloned().unwrap_or(Value::Null),
            "agentError": agent.err().map(|e| e.to_string()),
        }));
    }
    if command == "agent" {
        let op = need(args, 1, "the agent operation")?;
        let call_args: Value = match args.get(2) {
            Some(text) => serde_json::from_str(text)?,
            None => json!({}),
        };
        return Agent::connect(project)?.call(op, &call_args);
    }
    let editor = Editor::new(Bridge::connect(project)?.transport(transport));
    let xyz = |from: usize| -> Result<[f64; 3], Error> {
        Ok([number(args.get(from))?, number(args.get(from + 1))?, number(args.get(from + 2))?])
    };
    Ok(match command {
        "ping" => {
            editor.bridge().ping()?;
            json!({ "ok": true })
        }
        "level" => serde_json::to_value(editor.level()?)?,
        "open" => json!({ "ok": true, "level": editor.open_level(need(args, 1, "the level's name")?)? }),
        "save" => {
            editor.save_level()?;
            json!({ "ok": true })
        }
        "play" => {
            editor.play()?;
            json!({ "ok": true })
        }
        "stop" => {
            editor.stop()?;
            json!({ "ok": true })
        }
        "tool" => json!({ "ok": true, "open": editor.open_tool(need(args, 1, "the tool's name")?)? }),
        "selection" => serde_json::to_value(editor.selection()?)?,
        "find" => serde_json::to_value(editor.find(need(args, 1, "the name")?)?)?,
        "select" => json!({ "ok": true, "selected": editor.select(need(args, 1, "the name")?)? }),
        "place" => serde_json::to_value(editor.place(need(args, 1, "the name")?, xyz(2)?)?)?,
        "move" => json!({ "ok": true, "moved": editor.move_to(need(args, 1, "the name")?, xyz(2)?)? }),
        "console" => {
            if args.len() < 2 {
                return Err(Error::BadAnswer("missing the console command".into()));
            }
            json!({ "ok": true, "output": editor.console(&args[1..].join(" "))? })
        }
        "screenshot" => json!({ "ok": true, "output": editor.screenshot(std::path::Path::new(need(args, 1, "the picture's path")?))? }),
        other => return Err(Error::BadAnswer(format!("no command {other:?}; see hearth-link --help"))),
    })
}
