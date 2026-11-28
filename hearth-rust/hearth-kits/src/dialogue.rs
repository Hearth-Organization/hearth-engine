//! Conversations, written as plain text so anyone can write them:
//!
//! ```text
//! == gate
//! Guard: Halt! Who goes there?
//! * A friend. -> friend
//! * None of your business. -> rude
//! * (if has_pass) Show the pass. -> pass
//!
//! == friend
//! Guard: Then pass, friend. [set guard_likes_you]
//! -> end
//! ```
//!
//! `== name` starts a part; `Who: line` is said; `* choice -> part` is a choice (`(if flag)` / `(if not flag)` shows
//! it only then); `-> part` goes on without a choice; `[set flag]` and `[clear flag]` remember things for later
//! (the game and Hearth Script can read them). `-> end` ends the talk.

use std::collections::{BTreeSet, HashMap};

#[derive(Clone, Debug, PartialEq)]
pub enum Cond {
    Always,
    If(String),
    IfNot(String),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Choice {
    pub text: String,
    pub to: String,
    pub cond: Cond,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Line {
    pub who: String,
    pub text: String,
    pub set: Vec<String>,
    pub clear: Vec<String>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Part {
    pub lines: Vec<Line>,
    pub choices: Vec<Choice>,
    pub then: Option<String>,
}

#[derive(Clone, Debug, Default)]
pub struct Dialogue {
    pub parts: HashMap<String, Part>,
    pub first: String,
}

fn take_marks(text: &str) -> (String, Vec<String>, Vec<String>) {
    let (mut set, mut clear) = (Vec::new(), Vec::new());
    let mut out = String::new();
    let mut rest = text;
    while let Some(open) = rest.find('[') {
        let Some(close) = rest[open..].find(']') else { break };
        let inside = rest[open + 1..open + close].trim();
        if let Some(f) = inside.strip_prefix("set ") {
            set.push(f.trim().to_string());
        } else if let Some(f) = inside.strip_prefix("clear ") {
            clear.push(f.trim().to_string());
        } else {
            out.push_str(&rest[..open + close + 1]);
            rest = &rest[open + close + 1..];
            continue;
        }
        out.push_str(&rest[..open]);
        rest = &rest[open + close + 1..];
    }
    out.push_str(rest);
    (out.trim().to_string(), set, clear)
}

impl Dialogue {
    pub fn parse(text: &str) -> Result<Dialogue, String> {
        let mut d = Dialogue::default();
        let mut current: Option<String> = None;
        for (n, raw) in text.lines().enumerate() {
            let line = raw.trim();
            let bad = |m: &str| format!("line {}: {m}", n + 1);
            if line.is_empty() || line.starts_with("//") {
                continue;
            }
            if let Some(name) = line.strip_prefix("==") {
                let name = name.trim().to_string();
                if name.is_empty() {
                    return Err(bad("a part needs a name: == name"));
                }
                if d.parts.contains_key(&name) {
                    return Err(bad(&format!("there are two parts called {name}")));
                }
                if d.first.is_empty() {
                    d.first = name.clone();
                }
                d.parts.insert(name.clone(), Part::default());
                current = Some(name);
                continue;
            }
            let Some(part) = current.as_ref().and_then(|c| d.parts.get_mut(c)) else {
                return Err(bad("start with a part: == name"));
            };
            if let Some(rest) = line.strip_prefix('*') {
                let (text, to) = rest.split_once("->").ok_or_else(|| bad("a choice needs where it goes: * text -> part"))?;
                let mut text = text.trim();
                let mut cond = Cond::Always;
                if let Some(r) = text.strip_prefix("(if ") {
                    let (c, after) = r.split_once(')').ok_or_else(|| bad("close the (if …) with )"))?;
                    let c = c.trim();
                    cond = match c.strip_prefix("not ") {
                        Some(f) => Cond::IfNot(f.trim().to_string()),
                        None => Cond::If(c.to_string()),
                    };
                    text = after.trim();
                }
                part.choices.push(Choice { text: text.to_string(), to: to.trim().to_string(), cond });
            } else if let Some(to) = line.strip_prefix("->") {
                part.then = Some(to.trim().to_string());
            } else {
                let (who, said) = line.split_once(':').ok_or_else(|| bad("a line is `Who: what they say`"))?;
                let (text, set, clear) = take_marks(said);
                part.lines.push(Line { who: who.trim().to_string(), text, set, clear });
            }
        }
        if d.parts.is_empty() {
            return Err("the dialogue has no parts (start one with == name)".into());
        }
        for (name, p) in &d.parts {
            for to in p.choices.iter().map(|c| &c.to).chain(p.then.iter()) {
                if to != "end" && !d.parts.contains_key(to) {
                    return Err(format!("part {name} goes to {to}, which isn't written"));
                }
            }
        }
        Ok(d)
    }
}

/// A talk in progress. `flags` are what's been remembered (they outlive the talk: keep them in the save).
pub struct Talk<'a> {
    d: &'a Dialogue,
    part: Option<String>,
    line: usize,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Say {
    Line { who: String, text: String },
    Choose(Vec<String>),
    End,
}

impl<'a> Talk<'a> {
    pub fn start(d: &'a Dialogue, at: Option<&str>) -> Talk<'a> {
        Talk { d, part: Some(at.unwrap_or(&d.first).to_string()), line: 0 }
    }

    fn visible<'b>(&self, p: &'b Part, flags: &BTreeSet<String>) -> Vec<&'b Choice> {
        p.choices
            .iter()
            .filter(|c| match &c.cond {
                Cond::Always => true,
                Cond::If(f) => flags.contains(f),
                Cond::IfNot(f) => !flags.contains(f),
            })
            .collect()
    }

    /// What happens next: a line to show, choices to pick from, or the end.
    pub fn next(&mut self, flags: &mut BTreeSet<String>) -> Say {
        loop {
            let Some(name) = self.part.clone() else { return Say::End };
            if name == "end" {
                self.part = None;
                return Say::End;
            }
            let d = self.d;
            let Some(p) = d.parts.get(&name) else { return Say::End };
            if let Some(l) = p.lines.get(self.line) {
                self.line += 1;
                for f in &l.set {
                    flags.insert(f.clone());
                }
                for f in &l.clear {
                    flags.remove(f);
                }
                return Say::Line { who: l.who.clone(), text: l.text.clone() };
            }
            let shown = self.visible(p, flags);
            if !shown.is_empty() {
                return Say::Choose(shown.iter().map(|c| c.text.clone()).collect());
            }
            self.part = p.then.clone();
            self.line = 0;
        }
    }

    /// Picks the n-th choice shown (from 0).
    pub fn choose(&mut self, n: usize, flags: &BTreeSet<String>) -> Result<(), String> {
        let name = self.part.clone().ok_or("the talk is over")?;
        let d = self.d;
        let p = d.parts.get(&name).ok_or("the talk is over")?;
        let shown = self.visible(p, flags);
        let c = shown.get(n).ok_or_else(|| format!("there are only {} choices", shown.len()))?;
        self.part = Some(c.to.clone());
        self.line = 0;
        Ok(())
    }
}
