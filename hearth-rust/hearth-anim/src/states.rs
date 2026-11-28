//! An animation state machine: states, the rules for moving between them, and blending across a change.

use std::collections::HashMap;

/// A rule's test on the character's values ("speed > 0.1", "attacking", "not grounded").
#[derive(Clone, Debug, PartialEq)]
pub enum Test {
    Above(String, f32),
    Below(String, f32),
    On(String),
    Off(String),
}

impl Test {
    fn holds(&self, v: &Values) -> bool {
        match self {
            Test::Above(k, x) => v.numbers.get(k).copied().unwrap_or(0.0) > *x,
            Test::Below(k, x) => v.numbers.get(k).copied().unwrap_or(0.0) < *x,
            Test::On(k) => v.flags.get(k).copied().unwrap_or(false),
            Test::Off(k) => !v.flags.get(k).copied().unwrap_or(false),
        }
    }

    /// From plain text: "speed > 0.1", "speed < 3", "attacking", "not grounded".
    pub fn parse(s: &str) -> Result<Test, String> {
        let s = s.trim();
        if let Some((k, x)) = s.split_once('>') {
            return Ok(Test::Above(k.trim().into(), x.trim().parse().map_err(|_| format!("\"{s}\": the number after > isn't a number"))?));
        }
        if let Some((k, x)) = s.split_once('<') {
            return Ok(Test::Below(k.trim().into(), x.trim().parse().map_err(|_| format!("\"{s}\": the number after < isn't a number"))?));
        }
        if let Some(k) = s.strip_prefix("not ") {
            return Ok(Test::Off(k.trim().into()));
        }
        if s.is_empty() {
            return Err("an empty rule".into());
        }
        Ok(Test::On(s.into()))
    }
}

#[derive(Clone, Debug, Default)]
pub struct Values {
    pub numbers: HashMap<String, f32>,
    pub flags: HashMap<String, bool>,
}

impl Values {
    pub fn set(&mut self, k: &str, x: f32) {
        self.numbers.insert(k.into(), x);
    }
    pub fn flag(&mut self, k: &str, on: bool) {
        self.flags.insert(k.into(), on);
    }
}

#[derive(Clone, Debug)]
pub struct State {
    pub name: String,
    /// The animation clip it plays, how long it is, and whether it loops.
    pub clip: String,
    pub length: f32,
    pub looping: bool,
}

#[derive(Clone, Debug)]
pub struct Rule {
    /// From this state ("*" = any).
    pub from: String,
    pub to: String,
    /// All must hold.
    pub when: Vec<Test>,
    /// Seconds to blend over.
    pub blend: f32,
    /// Only once the clip has finished (an attack plays to its end).
    pub at_end: bool,
}

/// What to show this frame: one clip, or two being blended (`weight` of the newer one).
#[derive(Clone, Debug, PartialEq)]
pub struct Pose {
    pub clip: String,
    pub time: f32,
    pub from: Option<(String, f32)>,
    pub weight: f32,
}

#[derive(Clone, Debug)]
pub struct Machine {
    pub states: Vec<State>,
    pub rules: Vec<Rule>,
    current: usize,
    time: f32,
    previous: Option<(usize, f32)>,
    blend_left: f32,
    blend_total: f32,
}

impl Machine {
    pub fn new(states: Vec<State>, rules: Vec<Rule>) -> Result<Machine, String> {
        if states.is_empty() {
            return Err("an animation machine needs at least one state".into());
        }
        for r in &rules {
            for name in [&r.from, &r.to] {
                if name != "*" && !states.iter().any(|s| &s.name == name) {
                    return Err(format!("a rule uses the state \"{name}\", which isn't there"));
                }
            }
        }
        Ok(Machine { states, rules, current: 0, time: 0.0, previous: None, blend_left: 0.0, blend_total: 0.0 })
    }

    pub fn state(&self) -> &str {
        &self.states[self.current].name
    }

    fn index(&self, name: &str) -> Option<usize> {
        self.states.iter().position(|s| s.name == name)
    }

    /// Moves time on and follows the first rule that holds. Returns the pose to show.
    pub fn step(&mut self, dt: f32, v: &Values) -> Pose {
        self.time += dt;
        if let Some((_, t)) = &mut self.previous {
            *t += dt;
        }
        if self.blend_left > 0.0 {
            self.blend_left = (self.blend_left - dt).max(0.0);
            if self.blend_left == 0.0 {
                self.previous = None;
            }
        }
        let st = &self.states[self.current];
        let finished = !st.looping && self.time >= st.length;
        let here = st.name.clone();
        let next = self
            .rules
            .iter()
            .find(|r| (r.from == here || r.from == "*") && r.to != here && (!r.at_end || finished) && r.when.iter().all(|t| t.holds(v)))
            .and_then(|r| self.index(&r.to).map(|i| (i, r.blend)));
        if let Some((to, blend)) = next {
            self.previous = (blend > 0.0).then_some((self.current, self.time));
            self.current = to;
            self.time = 0.0;
            self.blend_left = blend;
            self.blend_total = blend;
        }
        self.pose()
    }

    pub fn pose(&self) -> Pose {
        let st = &self.states[self.current];
        let t = if st.looping { self.time % st.length.max(1e-6) } else { self.time.min(st.length) };
        let (from, weight) = match self.previous {
            Some((p, pt)) if self.blend_total > 0.0 => {
                let ps = &self.states[p];
                let ptime = if ps.looping { pt % ps.length.max(1e-6) } else { pt.min(ps.length) };
                (Some((ps.clip.clone(), ptime)), 1.0 - self.blend_left / self.blend_total)
            }
            _ => (None, 1.0),
        };
        Pose { clip: st.clip.clone(), time: t, from, weight }
    }
}
