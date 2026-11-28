//! Animate anything: tracks of keys on named values ("door.angle", "lamp.color.r"), with easing between keys.

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Ease {
    /// Jumps at the key.
    Step,
    Linear,
    /// Starts and ends gently.
    Smooth,
    In,
    Out,
}

fn ease(e: Ease, t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    match e {
        Ease::Step => 0.0,
        Ease::Linear => t,
        Ease::Smooth => t * t * (3.0 - 2.0 * t),
        Ease::In => t * t,
        Ease::Out => 1.0 - (1.0 - t) * (1.0 - t),
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Key {
    pub time: f32,
    pub value: f32,
    /// How it moves from this key to the next.
    pub ease: Ease,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Track {
    pub target: String,
    pub keys: Vec<Key>,
}

impl Track {
    pub fn new(target: &str, mut keys: Vec<Key>) -> Track {
        keys.sort_by(|a, b| a.time.total_cmp(&b.time));
        Track { target: target.into(), keys }
    }

    pub fn value(&self, t: f32) -> Option<f32> {
        let first = self.keys.first()?;
        if t <= first.time {
            return Some(first.value);
        }
        for w in self.keys.windows(2) {
            let (a, b) = (&w[0], &w[1]);
            if t < b.time {
                let span = (b.time - a.time).max(1e-6);
                return Some(a.value + (b.value - a.value) * ease(a.ease, (t - a.time) / span));
            }
        }
        self.keys.last().map(|k| k.value)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Repeat {
    Once,
    Loop,
    /// There and back.
    PingPong,
}

/// Events on the timeline ("play the creak sound at 0.2 s").
#[derive(Clone, Debug, PartialEq)]
pub struct Mark {
    pub time: f32,
    pub event: String,
}

#[derive(Clone, Debug)]
pub struct Timeline {
    pub tracks: Vec<Track>,
    pub marks: Vec<Mark>,
    pub length: f32,
    pub repeat: Repeat,
    pub time: f32,
    pub speed: f32,
}

impl Timeline {
    pub fn new(tracks: Vec<Track>, marks: Vec<Mark>, repeat: Repeat) -> Timeline {
        let length = tracks.iter().flat_map(|t| t.keys.iter().map(|k| k.time)).chain(marks.iter().map(|m| m.time)).fold(0.0, f32::max);
        Timeline { tracks, marks, length, repeat, time: 0.0, speed: 1.0 }
    }

    fn local(&self, t: f32) -> f32 {
        let len = self.length.max(1e-6);
        match self.repeat {
            Repeat::Once => t.clamp(0.0, len),
            Repeat::Loop => t.rem_euclid(len),
            Repeat::PingPong => {
                let p = t.rem_euclid(2.0 * len);
                if p > len {
                    2.0 * len - p
                } else {
                    p
                }
            }
        }
    }

    /// Moves time on: returns every value now, and the marks passed (in order).
    pub fn step(&mut self, dt: f32) -> (Vec<(String, f32)>, Vec<String>) {
        let before = self.time;
        self.time += dt * self.speed;
        let mut passed = Vec::new();
        if self.repeat == Repeat::Once {
            let (a, b) = (before.min(self.length), self.time.min(self.length));
            for m in &self.marks {
                if (m.time > a || (before == 0.0 && m.time == 0.0)) && m.time <= b {
                    passed.push(m.event.clone());
                }
            }
        } else {
            // Walk whole loops so a long step never skips a mark.
            let len = self.length.max(1e-6);
            let mut t = before;
            while t < self.time {
                let base = (t / len).floor() * len;
                let end = (base + len).min(self.time);
                for m in &self.marks {
                    let at = base + m.time;
                    if at > t - if t == 0.0 { 1e-9 } else { 0.0 } && at <= end {
                        passed.push(m.event.clone());
                    }
                }
                t = end;
                if end >= self.time {
                    break;
                }
            }
        }
        let lt = self.local(self.time);
        (self.tracks.iter().filter_map(|tr| tr.value(lt).map(|v| (tr.target.clone(), v))).collect(), passed)
    }

    pub fn finished(&self) -> bool {
        self.repeat == Repeat::Once && self.time >= self.length
    }
}
