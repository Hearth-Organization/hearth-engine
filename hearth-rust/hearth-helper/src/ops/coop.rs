//! Co-op test: this computer hosts; a second game window on this computer can join it.

use std::net::UdpSocket;
use std::time::Duration;

use serde_json::{json, Value};

use crate::ctx::{self, p};
use crate::util::*;

/// This computer's address on the local network (for the co-op invite). No packet is sent: connecting a UDP
/// socket only picks the outgoing network.
pub fn lan_address() -> String {
    UdpSocket::bind("0.0.0.0:0")
        .and_then(|s| {
            s.connect("192.168.1.1:9")?;
            s.local_addr()
        })
        .map(|a| a.ip().to_string())
        .unwrap_or_else(|_| "127.0.0.1".into())
}

fn command(role: &str, args: &Value, port: u16) -> Vec<String> {
    let level = args.get("level").and_then(Value::as_str).unwrap_or("levels/level0/level0.spawnable");
    let mut cmd = vec![
        p().launcher.display().to_string(),
        format!("--project-path={}", p().project.display()),
        if role == "host" {
            format!("--regset=/O3DE/Autoexec/ConsoleCommands/LoadLevel={level}")
        } else {
            format!("--regset=/O3DE/Autoexec/ConsoleCommands/connect=127.0.0.1:{port}")
        },
        format!("--regset=/O3DE/Autoexec/ConsoleCommands/r_width={}", int(args, "width", 1280)),
        format!("--regset=/O3DE/Autoexec/ConsoleCommands/r_height={}", int(args, "height", 720)),
        format!("--regset=/O3DE/Autoexec/ConsoleCommands/net_FakeLatencyMs={}", int(args, "lag_ms", 0)),
        format!("--regset=/O3DE/Autoexec/ConsoleCommands/net_FakeLossPercent={}", int(args, "loss_percent", 0)),
    ];
    if role == "host" {
        cmd.push(format!("--regset=/O3DE/Autoexec/ConsoleCommands/sv_port={port}"));
        cmd.push("--regset=/O3DE/Autoexec/ConsoleCommands/host=".into());
    }
    let seed = text(args, "seed").trim().to_string();
    if !seed.is_empty() && seed.chars().all(|c| c.is_ascii_digit()) {
        cmd.push(format!("--regset=/NoclipProtocol/Level0/Override/seed={seed}"));
        cmd.push("--regset=/NoclipProtocol/Level0/Override/newSeedEachRun=false".into());
    }
    if args.get("monsters") == Some(&json!(false)) {
        cmd.push("--regset=/NoclipProtocol/Monsters/Enabled=false".into());
    }
    cmd
}

pub fn op_coop_start(args: &Value) -> Value {
    if !p().launcher.exists() {
        return err(format!("no game launcher at {}; build first", p().launcher.display()));
    }
    if !gem_names().iter().any(|g| g == "Multiplayer") {
        return err("the Multiplayer engine part is off: turn it on in the Hub and rebuild");
    }
    op_coop_stop(&json!({}));
    let port = int(args, "port", 33450).clamp(1, 65535) as u16;
    let launch = read_json(&p().launch_file).unwrap_or(Value::Null);
    let env = session_env(launch.get("env").unwrap_or(&Value::Null));
    let host = match spawn(&own_scope(command("host", args, port)), &p().project, &env, &p().coop_log, true) {
        Ok(c) => c,
        Err(e) => return err(format!("couldn't start the host: {e}")),
    };
    log(&format!("co-op host started, pid {}, port {port}", host.id()));
    {
        let mut s = ctx::state();
        s.coop_host = Some(host);
        s.coop_port = port;
        s.coop_started = Some(now());
        s.coop_settings = args.clone();
    }
    if flag(args, "second_window", true) {
        std::thread::sleep(Duration::from_secs_f64(num(args, "join_delay", 8.0).max(0.0)));
        match spawn(&own_scope(command("client", args, port)), &p().project, &env, &p().coop_log2, true) {
            Ok(c) => {
                log(&format!("co-op second window started, pid {}", c.id()));
                ctx::state().coop_window2 = Some(c);
            }
            Err(e) => log(&format!("co-op second window didn't start: {e}")),
        }
    }
    json!({"ok": true, "invite": format!("connect {}:{port}", lan_address()), "port": port})
}

fn alive(c: &mut Option<std::process::Child>) -> Option<u32> {
    let child = c.as_mut()?;
    match child.try_wait() {
        Ok(None) => Some(child.id()),
        _ => None,
    }
}

fn addresses(line: &str) -> Vec<String> {
    line.split(|c: char| !(c.is_ascii_digit() || c == '.'))
        .filter(|t| {
            let parts: Vec<&str> = t.split('.').collect();
            parts.len() == 4 && parts.iter().all(|p| !p.is_empty() && p.len() <= 3)
        })
        .map(str::to_string)
        .collect()
}

pub fn op_coop_status(_args: &Value) -> Value {
    let (host, window2, port, started, settings) = {
        let mut s = ctx::state();
        (alive(&mut s.coop_host), alive(&mut s.coop_window2), s.coop_port, s.coop_started, s.coop_settings.clone())
    };
    let mut players = Vec::new();
    if let Some(pid) = host {
        players.push(json!({"name": "You", "where": "This computer · host", "state": "ready", "pid": pid, "mb": rss_mb(pid as i32)}));
    }
    if let Some(pid) = window2 {
        players.push(json!({"name": "Window 2", "where": "This computer · second game window", "state": "ready", "pid": pid, "mb": rss_mb(pid as i32)}));
    }
    let mut lines = Vec::new();
    let mut remote: Vec<(String, &str)> = Vec::new();
    let text_tail = if p().coop_log.exists() { tail(&p().coop_log, 400) } else { String::new() };
    for line in text_tail.lines() {
        let low = line.to_lowercase();
        if ["connect", "join", "accept", "host", "disconnect", "loading", "level"].iter().any(|w| low.contains(w)) {
            lines.push(last_chars(line.trim(), 160));
            for a in addresses(line) {
                if !a.starts_with("127.") && !a.starts_with("0.") {
                    let state = if low.contains("disconnect") { "left" } else { "ready" };
                    match remote.iter_mut().find(|(x, _)| *x == a) {
                        Some(r) => r.1 = state,
                        None => remote.push((a, state)),
                    }
                }
            }
        }
    }
    for (address, state) in remote {
        players.push(json!({"name": address, "where": "Another computer", "state": state}));
    }
    json!({"ok": true, "running": host.is_some(), "players": players, "log": lines[lines.len().saturating_sub(30)..],
           "invite": format!("connect {}:{port}", lan_address()), "started": started, "settings": settings,
           "logs": [p().coop_log.display().to_string(), p().coop_log2.display().to_string()]})
}

pub fn op_coop_stop(_args: &Value) -> Value {
    let mut stopped = Vec::new();
    for key in ["window2", "host"] {
        let child = {
            let mut s = ctx::state();
            if key == "host" { s.coop_host.take() } else { s.coop_window2.take() }
        };
        if let Some(mut c) = child {
            if let Ok(None) = c.try_wait() {
                kill(c.id() as i32, libc::SIGTERM);
                let deadline = std::time::Instant::now() + Duration::from_secs(5);
                while std::time::Instant::now() < deadline && matches!(c.try_wait(), Ok(None)) {
                    std::thread::sleep(Duration::from_millis(100));
                }
                if let Ok(None) = c.try_wait() {
                    let _ = c.kill();
                }
                let _ = c.wait();
                stopped.push(key);
            }
        }
    }
    json!({"ok": true, "stopped": stopped})
}
