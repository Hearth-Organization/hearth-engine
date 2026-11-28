use std::io::{BufRead, BufReader, Write};
use std::net::{SocketAddr, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde::Deserialize;
use serde_json::{json, Map, Value};

use crate::error::{Error, Result};
use crate::session::Session;

/// How a request reaches the editor's bridge.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Transport {
    /// The localhost socket; the file inbox if the socket can't be opened.
    Auto,
    /// Only the localhost socket.
    Socket,
    /// Only the bridge's file inbox (for when a sandbox blocks connections).
    Files,
}

/// The bridge's answer: `{"id": ..., "ok": true/false, "output": "...", "error": "..."}`.
#[derive(Debug, Clone, Deserialize, PartialEq)]
pub struct Reply {
    pub ok: bool,
    #[serde(default)]
    pub output: Option<String>,
    #[serde(default)]
    pub error: Option<String>,
    #[serde(default)]
    pub id: Option<Value>,
}

impl Reply {
    pub fn output(&self) -> &str {
        self.output.as_deref().unwrap_or("")
    }

    /// The answer as a result: the editor's own words when it said no.
    pub fn into_result(self) -> Result<Reply> {
        if self.ok {
            Ok(self)
        } else {
            Err(Error::Editor(self.error.clone().unwrap_or_else(|| "it didn't say why".into())))
        }
    }
}

/// A connection to the editor's bridge for one project.
#[derive(Debug, Clone)]
pub struct Bridge {
    project: PathBuf,
    session: Session,
    transport: Transport,
    timeout: Duration,
}

static NEXT_ID: AtomicU64 = AtomicU64::new(1);

fn request_id() -> String {
    let nanos = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0);
    format!("link-{}-{}-{}", std::process::id(), nanos, NEXT_ID.fetch_add(1, Ordering::Relaxed))
}

impl Bridge {
    /// Reads the project's bridge session. Fails with [`Error::EditorNotRunning`] when the editor isn't open.
    pub fn connect(project: &Path) -> Result<Bridge> {
        Ok(Bridge {
            project: project.to_path_buf(),
            session: Session::read(project)?,
            transport: Transport::Auto,
            timeout: Duration::from_secs(60),
        })
    }

    /// For tests and tools that already have a session.
    pub fn with_session(project: &Path, session: Session) -> Bridge {
        Bridge { project: project.to_path_buf(), session, transport: Transport::Auto, timeout: Duration::from_secs(60) }
    }

    pub fn transport(mut self, transport: Transport) -> Self {
        self.transport = transport;
        self
    }

    pub fn timeout(mut self, timeout: Duration) -> Self {
        self.timeout = timeout;
        self
    }

    pub fn project(&self) -> &Path {
        &self.project
    }

    /// Sends one request (`op` plus its fields) and waits for the editor's answer.
    pub fn request(&self, op: &str, fields: Map<String, Value>) -> Result<Reply> {
        let id = request_id();
        let mut body = fields;
        body.insert("token".into(), json!(self.session.token));
        body.insert("id".into(), json!(id));
        body.insert("op".into(), json!(op));
        let body = Value::Object(body);
        match self.transport {
            Transport::Socket => self.over_socket(&body, &id),
            Transport::Files => self.over_files(&body, &id),
            Transport::Auto => match self.over_socket(&body, &id) {
                Err(Error::Io(e)) if matches!(
                    e.kind(),
                    std::io::ErrorKind::ConnectionRefused | std::io::ErrorKind::PermissionDenied
                ) => self.over_files(&body, &id),
                other => other,
            },
        }
    }

    pub fn ping(&self) -> Result<()> {
        self.request("ping", Map::new())?.into_result().map(|_| ())
    }

    fn over_socket(&self, body: &Value, id: &str) -> Result<Reply> {
        let address = SocketAddr::from(([127, 0, 0, 1], self.session.port));
        let mut stream = TcpStream::connect_timeout(&address, Duration::from_secs(3))?;
        stream.set_read_timeout(Some(self.timeout))?;
        stream.set_write_timeout(Some(Duration::from_secs(10)))?;
        let mut line = serde_json::to_vec(body)?;
        line.push(b'\n');
        stream.write_all(&line)?;
        stream.flush()?;
        let mut reader = BufReader::new(stream);
        let mut answer = String::new();
        match reader.read_line(&mut answer) {
            Ok(0) => return Err(Error::BadAnswer("the editor closed the connection".into())),
            Ok(_) => {}
            Err(e) if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => {
                return Err(Error::Timeout(format!("the editor didn't answer within {} s", self.timeout.as_secs())))
            }
            Err(e) => return Err(Error::Io(e)),
        }
        let reply: Reply = serde_json::from_str(answer.trim())?;
        check_id(&reply, id)?;
        Ok(reply)
    }

    fn over_files(&self, body: &Value, id: &str) -> Result<Reply> {
        let folder = Session::folder(&self.project);
        let inbox = folder.join("inbox");
        let outbox = folder.join("outbox");
        std::fs::create_dir_all(&inbox)?;
        let name = format!("{id}.json");
        let tmp = inbox.join(format!("{id}.tmp"));
        std::fs::write(&tmp, serde_json::to_vec(body)?)?;
        std::fs::rename(&tmp, inbox.join(&name))?;
        let answer = outbox.join(&name);
        let deadline = Instant::now() + self.timeout;
        while Instant::now() < deadline {
            if let Ok(text) = std::fs::read_to_string(&answer) {
                if let Ok(reply) = serde_json::from_str::<Reply>(&text) {
                    let _ = std::fs::remove_file(&answer);
                    check_id(&reply, id)?;
                    return Ok(reply);
                }
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        // Don't leave a request behind for the editor to run later, when nobody is waiting for it.
        let _ = std::fs::remove_file(inbox.join(&name));
        Err(Error::Timeout(format!("the editor didn't answer within {} s", self.timeout.as_secs())))
    }
}

fn check_id(reply: &Reply, id: &str) -> Result<()> {
    match &reply.id {
        Some(Value::String(s)) if s != id => Err(Error::BadAnswer(format!("an answer to another request ({s})"))),
        _ => Ok(()),
    }
}
