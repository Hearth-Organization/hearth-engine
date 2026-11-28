use std::fmt;

/// What went wrong, in words that can be shown to the person.
#[derive(Debug)]
pub enum Error {
    /// The editor isn't open, or its bridge hasn't started yet.
    EditorNotRunning(String),
    /// The editor answered, but said no.
    Editor(String),
    /// The Hearth Engine agent isn't running or refused.
    Agent(String),
    /// No answer in time.
    Timeout(String),
    /// Reading or writing a file or the connection failed.
    Io(std::io::Error),
    /// An answer that couldn't be read.
    BadAnswer(String),
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::EditorNotRunning(why) => write!(f, "The editor isn't open yet ({why})."),
            Error::Editor(why) => write!(f, "The editor said: {why}"),
            Error::Agent(why) => write!(f, "The Hearth Engine helper said: {why}"),
            Error::Timeout(what) => write!(f, "No answer in time: {what}"),
            Error::Io(e) => write!(f, "{e}"),
            Error::BadAnswer(why) => write!(f, "An answer that couldn't be read: {why}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Error::Io(e)
    }
}

impl From<serde_json::Error> for Error {
    fn from(e: serde_json::Error) -> Self {
        Error::BadAnswer(e.to_string())
    }
}
