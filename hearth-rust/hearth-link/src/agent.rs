use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::path::{Path, PathBuf};
use std::time::Duration;

use serde_json::Value;

use crate::error::{Error, Result};

/// The Hearth Engine agent's localhost API (`scripts/hearth/agent/hearth_agent.py`): building the game, starting
/// and stopping the editor and the game, logs, crash reports. Every call needs the agent's token, in
/// `<project>/user/HearthAgent/token`.
#[derive(Debug, Clone)]
pub struct Agent {
    port: u16,
    token: String,
    timeout: Duration,
}

pub const AGENT_PORT: u16 = 47830;

impl Agent {
    pub fn token_file(project: &Path) -> PathBuf {
        project.join("user").join("HearthAgent").join("token")
    }

    pub fn connect(project: &Path) -> Result<Agent> {
        let path = Self::token_file(project);
        let token = std::fs::read_to_string(&path)
            .map_err(|e| Error::Agent(format!("not set up for this project (no {}: {e})", path.display())))?
            .trim()
            .to_string();
        Ok(Agent { port: AGENT_PORT, token, timeout: Duration::from_secs(120) })
    }

    pub fn with(port: u16, token: &str) -> Agent {
        Agent { port, token: token.to_string(), timeout: Duration::from_secs(120) }
    }

    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.timeout = timeout;
        self
    }

    /// Runs one of the agent's operations ("status", "build", "editor_start", "logs"...) with its arguments.
    pub fn call(&self, op: &str, args: &Value) -> Result<Value> {
        if op.is_empty() || !op.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
            return Err(Error::Agent(format!("{op:?} isn't one of its operations")));
        }
        let body = serde_json::to_vec(args)?;
        let address = SocketAddr::from(([127, 0, 0, 1], self.port));
        let mut stream = TcpStream::connect_timeout(&address, Duration::from_secs(3))
            .map_err(|e| Error::Agent(format!("not running ({e})")))?;
        stream.set_read_timeout(Some(self.timeout))?;
        let head = format!(
            "POST /{op} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nX-Hearth-Token: {token}\r\n\
             Content-Type: application/json\r\nContent-Length: {len}\r\nConnection: close\r\n\r\n",
            port = self.port,
            token = self.token,
            len = body.len()
        );
        stream.write_all(head.as_bytes())?;
        stream.write_all(&body)?;
        let mut answer = Vec::new();
        stream.read_to_end(&mut answer).map_err(|e| match e.kind() {
            std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut => {
                Error::Timeout(format!("the agent didn't answer {op} within {} s", self.timeout.as_secs()))
            }
            _ => Error::Io(e),
        })?;
        let (status, body) = split_http(&answer)?;
        if status != 200 {
            let why = serde_json::from_slice::<Value>(body)
                .ok()
                .and_then(|v| v.get("error").and_then(Value::as_str).map(str::to_string))
                .unwrap_or_else(|| format!("HTTP {status}"));
            return Err(Error::Agent(why));
        }
        Ok(serde_json::from_slice(body)?)
    }
}

/// The status code and the body of an HTTP/1.x answer.
fn split_http(answer: &[u8]) -> Result<(u16, &[u8])> {
    let end = answer
        .windows(4)
        .position(|w| w == b"\r\n\r\n")
        .ok_or_else(|| Error::BadAnswer("the agent's answer had no end of headers".into()))?;
    let head = std::str::from_utf8(&answer[..end]).map_err(|e| Error::BadAnswer(e.to_string()))?;
    let status = head
        .split_whitespace()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .ok_or_else(|| Error::BadAnswer(format!("no status in {:?}", head.lines().next())))?;
    let mut body = &answer[end + 4..];
    let length = head.lines().find_map(|l| {
        let (k, v) = l.split_once(':')?;
        if k.trim().eq_ignore_ascii_case("content-length") {
            v.trim().parse::<usize>().ok()
        } else {
            None
        }
    });
    if let Some(len) = length {
        body = &body[..len.min(body.len())];
    }
    Ok((status, body))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_an_http_answer() {
        let a = b"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n{\"ok\": true}extra";
        let (status, body) = split_http(a).unwrap();
        assert_eq!(status, 200);
        assert_eq!(body, b"{\"ok\": true");
    }

    #[test]
    fn only_plain_operation_names() {
        let agent = Agent::with(1, "t");
        assert!(matches!(agent.call("../status", &Value::Null), Err(Error::Agent(_))));
        assert!(matches!(agent.call("status\r\nX: y", &Value::Null), Err(Error::Agent(_))));
    }
}
