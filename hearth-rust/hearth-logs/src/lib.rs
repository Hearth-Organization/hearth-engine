//! Reads Hearth Engine's logs (the editor's, the game's, the Asset Processor's) and says what went wrong in plain
//! words: errors and warnings grouped so one problem repeated a thousand times is one line, whether it crashed and
//! what happened just before, and what usually fixes the common problems.

use std::collections::HashMap;

use serde_json::json;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Kind {
    Warning,
    Error,
    Assert,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Problem {
    pub kind: Kind,
    /// The part of the engine it came from ("Shader", "AssetProcessor"), when the log says.
    pub source: String,
    pub message: String,
    /// Where in the code, when the log says ("Gem/Source/Door.cpp(42)").
    pub at: Option<String>,
    pub first_line: usize,
    pub times: usize,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Hint {
    pub what: String,
    pub fix: String,
}

#[derive(Clone, Debug, Default)]
pub struct Report {
    pub lines: usize,
    pub problems: Vec<Problem>,
    pub crashed: bool,
    pub crash_line: Option<usize>,
    /// The lines just before the crash.
    pub before_crash: Vec<String>,
    pub hints: Vec<Hint>,
}

/// `<12:03:04> (Window) - text` → (window, text). Other lines come back whole.
fn split(line: &str) -> (String, String) {
    let rest = line.trim_start();
    let rest = match rest.strip_prefix('<').and_then(|r| r.split_once('>')) {
        Some((_, r)) => r.trim_start(),
        None => rest,
    };
    if let Some(r) = rest.strip_prefix('(') {
        if let Some((w, t)) = r.split_once(") - ") {
            return (w.trim().to_string(), t.trim().to_string());
        }
    }
    (String::new(), rest.trim().to_string())
}

/// The message with numbers and addresses taken out, so the same problem groups together.
fn shape(msg: &str) -> String {
    let mut out = String::new();
    let mut last_digit = false;
    for c in msg.chars() {
        if c.is_ascii_digit() {
            if !last_digit {
                out.push('#');
            }
            last_digit = true;
        } else {
            last_digit = false;
            out.extend(c.to_lowercase());
        }
    }
    out.replace("0x#", "#")
}

const CRASH: [&str; 8] = ["SIGSEGV", "Segmentation fault", "SIGABRT", "Aborted (core dumped)", "terminate called", "==== Callstack", "Fatal signal", "Unhandled exception"];

const HINTS: [(&[&str], &str, &str); 9] = [
    (&["failed to compile shader", "shader compilation failed", "azslc", "dxc failed"], "A shader didn't compile, so some things may draw wrong or not at all.", "Open the Asset Processor and look at the failed shader; a material or shader file was likely changed by hand."),
    (&["could not find asset", "unable to find asset", "missing asset", "asset not found", "failed to load asset"], "A file the level uses is missing.", "Check the file is in the project and the Asset Processor processed it; a renamed or deleted file is the usual cause."),
    (&["out of memory", "bad_alloc", "failed to allocate"], "The computer ran out of memory.", "Close other programs, lower texture quality, or turn on Hearth Engine's game tabs pausing for background games."),
    (&["vk_error_device_lost", "device lost", "device_removed", "gpu hang"], "The graphics driver stopped (the GPU crashed).", "Update the graphics driver; if it keeps happening, lower ray tracing and resolution."),
    (&["physx"], "The physics system complained.", "Usually a shape that's too thin, too big or not closed: run the model through Import Checks."),
    (&["traceback (most recent call last)"], "A Python tool script failed.", "The lines after 'Traceback' say which script and line."),
    (&["hearth script"], "A Hearth Script has a mistake.", "The message says the file and line; the last good version keeps running until it's fixed."),
    (&["process exited with code", "assetbuilder crashed", "job failed"], "Processing an asset failed.", "Open the Asset Processor, find the red job, and read its log."),
    (&["permission denied", "read-only file system"], "Hearth Engine couldn't write a file.", "Check the project folder is yours and not on a read-only drive."),
];

pub fn read(text: &str) -> Report {
    let lines: Vec<&str> = text.lines().collect();
    let mut r = Report { lines: lines.len(), ..Report::default() };
    let mut groups: HashMap<(Kind, String), usize> = HashMap::new();
    let mut i = 0;
    while i < lines.len() {
        let (window, body) = split(lines[i]);
        let lower = body.to_lowercase();
        if r.crash_line.is_none() && CRASH.iter().any(|c| lines[i].contains(c)) {
            r.crashed = true;
            r.crash_line = Some(i + 1);
            let start = i.saturating_sub(8);
            r.before_crash = lines[start..i].iter().map(|l| l.to_string()).collect();
        }
        let header = [("Trace::Assert", Kind::Assert), ("Trace::Error", Kind::Error), ("Trace::Warning", Kind::Warning)].iter().find(|(h, _)| body.contains(h)).map(|(_, k)| *k);
        let (kind, at, message, used) = if let Some(kind) = header {
            // Trace::Error / file(line): 'function' / the message.
            let at = lines.get(i + 1).map(|l| split(l).1).filter(|l| l.contains('(') && l.contains("):"));
            let skip = if at.is_some() { 2 } else { 1 };
            let message = lines.get(i + skip).map(|l| split(l).1).unwrap_or_default();
            let at = at.map(|a| a.split("):").next().map(|s| format!("{s})")).unwrap_or(a));
            (Some(kind), at, message, skip + 1)
        } else if lower.starts_with("[error]") || lower.contains(" error:") || lower.starts_with("error:") || lower.starts_with("error ") {
            (Some(Kind::Error), None, body.clone(), 1)
        } else if lower.starts_with("[warning]") || lower.starts_with("warning:") || lower.contains(" warning:") {
            (Some(Kind::Warning), None, body.clone(), 1)
        } else {
            (None, None, String::new(), 1)
        };
        if let Some(kind) = kind {
            let key = (kind, shape(&message));
            match groups.get(&key) {
                Some(&k) => r.problems[k].times += 1,
                None => {
                    groups.insert(key, r.problems.len());
                    r.problems.push(Problem { kind, source: window, message, at, first_line: i + 1, times: 1 });
                }
            }
        }
        i += used;
    }
    // Worst first, then most often.
    r.problems.sort_by(|a, b| b.kind.cmp(&a.kind).then(b.times.cmp(&a.times)).then(a.first_line.cmp(&b.first_line)));
    let all = text.to_lowercase();
    for (needles, what, fix) in HINTS {
        if needles.iter().any(|n| all.contains(n)) {
            r.hints.push(Hint { what: what.to_string(), fix: fix.to_string() });
        }
    }
    r
}

impl Report {
    pub fn count(&self, kind: Kind) -> usize {
        self.problems.iter().filter(|p| p.kind == kind).map(|p| p.times).sum()
    }

    pub fn summary(&self) -> String {
        let mut s = String::new();
        if self.crashed {
            s += &format!("It crashed (line {}).\n", self.crash_line.unwrap_or(0));
        }
        s += &format!("{} errors, {} warnings, {} asserts.\n", self.count(Kind::Error), self.count(Kind::Warning), self.count(Kind::Assert));
        for p in self.problems.iter().take(10) {
            let word = match p.kind {
                Kind::Assert => "Assert",
                Kind::Error => "Error",
                Kind::Warning => "Warning",
            };
            let times = if p.times > 1 { format!(" ({} times)", p.times) } else { String::new() };
            let from = if p.source.is_empty() { String::new() } else { format!(" [{}]", p.source) };
            s += &format!("{word}{from}: {}{times}\n", p.message);
        }
        for h in &self.hints {
            s += &format!("- {} {}\n", h.what, h.fix);
        }
        s
    }

    pub fn to_json(&self) -> serde_json::Value {
        json!({
            "lines": self.lines,
            "crashed": self.crashed,
            "crash_line": self.crash_line,
            "before_crash": self.before_crash,
            "errors": self.count(Kind::Error),
            "warnings": self.count(Kind::Warning),
            "asserts": self.count(Kind::Assert),
            "problems": self.problems.iter().map(|p| json!({
                "kind": format!("{:?}", p.kind).to_lowercase(), "source": p.source, "message": p.message,
                "at": p.at, "first_line": p.first_line, "times": p.times })).collect::<Vec<_>>(),
            "hints": self.hints.iter().map(|h| json!({"what": h.what, "fix": h.fix})).collect::<Vec<_>>(),
        })
    }
}
