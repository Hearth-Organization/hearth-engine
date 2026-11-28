use std::fmt;

/// Where something is in a script: line and column, counted from 1.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Where {
    pub line: u32,
    pub col: u32,
}

/// A mistake in a script, in plain words, with where it is and (often) how to fix it.
#[derive(Debug, Clone, PartialEq)]
pub struct ScriptError {
    pub at: Where,
    pub message: String,
    pub hint: Option<String>,
    /// The script's name, when known ("door.hs").
    pub script: Option<String>,
}

impl ScriptError {
    pub fn at(at: Where, message: impl Into<String>) -> ScriptError {
        ScriptError { at, message: message.into(), hint: None, script: None }
    }

    pub fn hint(mut self, hint: impl Into<String>) -> ScriptError {
        self.hint = Some(hint.into());
        self
    }

    pub fn in_script(mut self, name: &str) -> ScriptError {
        if self.script.is_none() {
            self.script = Some(name.to_string());
        }
        self
    }
}

impl fmt::Display for ScriptError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if let Some(s) = &self.script {
            write!(f, "{s}, ")?;
        }
        write!(f, "line {}: {}", self.at.line, self.message)?;
        if let Some(h) = &self.hint {
            write!(f, " ({h})")?;
        }
        Ok(())
    }
}

impl std::error::Error for ScriptError {}

/// The closest known name to a misspelt one, for "did you mean".
pub fn closest<'a>(wanted: &str, known: impl Iterator<Item = &'a str>) -> Option<String> {
    let mut best: Option<(usize, &str)> = None;
    for k in known {
        let d = distance(wanted, k);
        let limit = (wanted.chars().count().max(3) / 3).max(1);
        if d <= limit && best.map(|(bd, _)| d < bd).unwrap_or(true) {
            best = Some((d, k));
        }
    }
    best.map(|(_, k)| k.to_string())
}

fn distance(a: &str, b: &str) -> usize {
    let a: Vec<char> = a.chars().collect();
    let b: Vec<char> = b.chars().collect();
    let mut prev: Vec<usize> = (0..=b.len()).collect();
    for i in 1..=a.len() {
        let mut cur = vec![i; b.len() + 1];
        for j in 1..=b.len() {
            let cost = usize::from(a[i - 1] != b[j - 1]);
            cur[j] = (prev[j] + 1).min(cur[j - 1] + 1).min(prev[j - 1] + cost);
        }
        prev = cur;
    }
    prev[b.len()]
}
