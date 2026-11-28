//! Quests: objectives that count what happens ("collect 5 wood", "talk to the guard"), and what you get for them.

use std::collections::HashMap;

#[derive(Clone, Debug, PartialEq)]
pub struct Objective {
    /// What the player reads ("Collect wood").
    pub text: String,
    /// The game event that counts toward it ("picked_up:wood", "talked:guard", "killed:wolf").
    pub event: String,
    pub needed: u32,
    pub done: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum State {
    Locked,
    Open,
    Active,
    Complete,
}

#[derive(Clone, Debug)]
pub struct Quest {
    pub id: String,
    pub name: String,
    pub objectives: Vec<Objective>,
    /// Quests that must be complete first.
    pub after: Vec<String>,
    /// What it gives (item or resource name, how many).
    pub rewards: Vec<(String, u32)>,
    /// Objectives in order (one at a time) or all at once.
    pub in_order: bool,
    pub state: State,
}

impl Quest {
    pub fn new(id: &str, name: &str) -> Quest {
        Quest { id: id.into(), name: name.into(), objectives: Vec::new(), after: Vec::new(), rewards: Vec::new(), in_order: false, state: State::Locked }
    }

    pub fn objective(mut self, text: &str, event: &str, needed: u32) -> Quest {
        self.objectives.push(Objective { text: text.into(), event: event.into(), needed: needed.max(1), done: 0 });
        self
    }

    pub fn reward(mut self, what: &str, n: u32) -> Quest {
        self.rewards.push((what.into(), n));
        self
    }

    pub fn after(mut self, id: &str) -> Quest {
        self.after.push(id.into());
        self
    }

    pub fn in_order(mut self) -> Quest {
        self.in_order = true;
        self
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Note {
    Opened(String),
    Started(String),
    Progress { quest: String, objective: String, done: u32, needed: u32 },
    Completed { quest: String, rewards: Vec<(String, u32)> },
}

#[derive(Default)]
pub struct Journal {
    pub quests: Vec<Quest>,
    index: HashMap<String, usize>,
}

impl Journal {
    pub fn add(&mut self, q: Quest) -> Result<(), String> {
        if self.index.contains_key(&q.id) {
            return Err(format!("there's already a quest called {}", q.id));
        }
        self.index.insert(q.id.clone(), self.quests.len());
        self.quests.push(q);
        Ok(())
    }

    pub fn get(&self, id: &str) -> Option<&Quest> {
        self.index.get(id).map(|&i| &self.quests[i])
    }

    /// Opens every quest whose earlier quests are done. Call after adding quests and after each completion.
    pub fn refresh(&mut self) -> Vec<Note> {
        let mut notes = Vec::new();
        let complete: std::collections::HashSet<String> = self.quests.iter().filter(|q| q.state == State::Complete).map(|q| q.id.clone()).collect();
        for q in self.quests.iter_mut().filter(|q| q.state == State::Locked) {
            if q.after.iter().all(|a| complete.contains(a)) {
                q.state = State::Open;
                notes.push(Note::Opened(q.id.clone()));
            }
        }
        notes
    }

    pub fn start(&mut self, id: &str) -> Result<Note, String> {
        let &i = self.index.get(id).ok_or_else(|| format!("there's no quest called {id}"))?;
        match self.quests[i].state {
            State::Open => {
                self.quests[i].state = State::Active;
                Ok(Note::Started(id.into()))
            }
            State::Locked => Err(format!("{} isn't open yet", self.quests[i].name)),
            _ => Err(format!("{} has already been started", self.quests[i].name)),
        }
    }

    /// Something happened in the game ("picked_up:wood"): counts it for every active quest that wants it.
    pub fn happened(&mut self, event: &str, times: u32) -> Vec<Note> {
        let mut notes = Vec::new();
        let mut completed = false;
        for q in self.quests.iter_mut().filter(|q| q.state == State::Active) {
            let in_order = q.in_order;
            for o in q.objectives.iter_mut().filter(|o| o.done < o.needed) {
                if o.event == event {
                    o.done = (o.done + times).min(o.needed);
                    notes.push(Note::Progress { quest: q.id.clone(), objective: o.text.clone(), done: o.done, needed: o.needed });
                }
                if in_order {
                    break; // only the first unfinished one counts
                }
            }
            if q.objectives.iter().all(|o| o.done >= o.needed) {
                q.state = State::Complete;
                notes.push(Note::Completed { quest: q.id.clone(), rewards: q.rewards.clone() });
                completed = true;
            }
        }
        if completed {
            notes.extend(self.refresh());
        }
        notes
    }
}
