//! A 2D platformer character on a tile map. The numbers players feel are all here and named: how fast it runs,
//! how high it jumps, how long after leaving a ledge a jump still works ("coyote time"), and how early a jump press
//! is remembered before landing ("jump buffer"). Letting go of jump early makes a short hop.

#[derive(Clone, Debug)]
pub struct Tiles {
    pub width: i32,
    pub height: i32,
    /// Metres per tile.
    pub size: f32,
    solid: Vec<bool>,
}

impl Tiles {
    /// Rows of text, top row first: `#` is solid. y grows upward in the world.
    pub fn from_text(rows: &str, size: f32) -> Tiles {
        let lines: Vec<&str> = rows.lines().filter(|l| !l.trim().is_empty()).collect();
        let width = lines.iter().map(|l| l.len()).max().unwrap_or(0) as i32;
        let height = lines.len() as i32;
        let mut solid = vec![false; (width * height) as usize];
        for (row, l) in lines.iter().enumerate() {
            let y = height - 1 - row as i32;
            for (x, ch) in l.chars().enumerate() {
                if ch == '#' {
                    solid[(y * width + x as i32) as usize] = true;
                }
            }
        }
        Tiles { width, height, size, solid }
    }

    /// Outside the map counts as solid at the sides and bottom (nothing falls out), open above.
    pub fn solid(&self, x: i32, y: i32) -> bool {
        if y >= self.height {
            return false;
        }
        if x < 0 || x >= self.width || y < 0 {
            return true;
        }
        self.solid[(y * self.width + x) as usize]
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Feel {
    pub run_speed: f32,
    /// Seconds to reach full speed, and to stop.
    pub speed_up: f32,
    pub slow_down: f32,
    /// How high (metres) a full jump goes, and how long (seconds) it takes to get there.
    pub jump_height: f32,
    pub jump_time: f32,
    /// Letting go early cuts the rise to this share.
    pub short_hop: f32,
    pub coyote_time: f32,
    pub jump_buffer: f32,
    pub max_fall: f32,
    /// How much control there is in the air (1 = as on the ground).
    pub air_control: f32,
}

impl Default for Feel {
    fn default() -> Feel {
        Feel { run_speed: 7.0, speed_up: 0.1, slow_down: 0.08, jump_height: 3.0, jump_time: 0.4, short_hop: 0.45, coyote_time: 0.1, jump_buffer: 0.12, max_fall: 20.0, air_control: 0.7 }
    }
}

impl Feel {
    pub fn gravity(&self) -> f32 {
        2.0 * self.jump_height / (self.jump_time * self.jump_time)
    }
    pub fn jump_speed(&self) -> f32 {
        self.gravity() * self.jump_time
    }
}

/// What the player is pressing this step: `run` from -1 (left) to 1 (right).
#[derive(Clone, Copy, Debug, Default)]
pub struct Input {
    pub run: f32,
    pub jump_held: bool,
}

#[derive(Clone, Debug)]
pub struct Character {
    /// The middle of its feet.
    pub x: f32,
    pub y: f32,
    pub vx: f32,
    pub vy: f32,
    pub half_width: f32,
    pub height: f32,
    pub on_ground: bool,
    pub feel: Feel,
    since_ground: f32,
    since_press: f32,
    was_held: bool,
    jumping: bool,
}

/// What happened this step, for sounds and animation.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Happened {
    pub jumped: bool,
    pub landed: bool,
    pub bumped_head: bool,
}

impl Character {
    pub fn new(x: f32, y: f32) -> Character {
        Character { x, y, vx: 0.0, vy: 0.0, half_width: 0.4, height: 1.8, on_ground: false, feel: Feel::default(), since_ground: f32::MAX, since_press: f32::MAX, was_held: false, jumping: false }
    }

    pub fn step(&mut self, tiles: &Tiles, input: Input, dt: f32) -> Happened {
        let f = self.feel.clone();
        let mut h = Happened::default();
        let pressed = input.jump_held && !self.was_held;
        self.was_held = input.jump_held;
        self.since_press = if pressed { 0.0 } else { self.since_press + dt };
        self.since_ground = if self.on_ground { 0.0 } else { self.since_ground + dt };

        // Running: speed up toward the target, slow down when let go.
        let target = input.run.clamp(-1.0, 1.0) * f.run_speed;
        let control = if self.on_ground { 1.0 } else { f.air_control };
        let rate = (if target.abs() > 0.01 { f.run_speed / f.speed_up.max(1e-3) } else { f.run_speed / f.slow_down.max(1e-3) }) * control;
        let dv = (target - self.vx).clamp(-rate * dt, rate * dt);
        self.vx += dv;

        // Jumping, with coyote time and the buffer.
        if self.since_press <= f.jump_buffer && self.since_ground <= f.coyote_time && !self.jumping {
            self.vy = f.jump_speed();
            self.jumping = true;
            self.on_ground = false;
            self.since_ground = f32::MAX;
            self.since_press = f32::MAX;
            h.jumped = true;
        }
        if self.jumping && !input.jump_held && self.vy > f.jump_speed() * f.short_hop {
            self.vy = f.jump_speed() * f.short_hop;
        }
        self.vy = (self.vy - f.gravity() * dt).max(-f.max_fall);

        // Move one axis at a time, stopping at tiles.
        self.x += self.vx * dt;
        if self.blocked(tiles) {
            self.x -= self.vx * dt;
            self.vx = 0.0;
        }
        let was_ground = self.on_ground;
        self.y += self.vy * dt;
        self.on_ground = false;
        if self.blocked(tiles) {
            if self.vy < 0.0 {
                // Land on top of the tile below.
                let feet_tile = (self.y / tiles.size).floor() + 1.0;
                self.y = feet_tile * tiles.size;
                if self.blocked(tiles) {
                    self.y -= self.vy * dt;
                }
                self.on_ground = true;
                self.jumping = false;
                h.landed = !was_ground;
            } else {
                self.y -= self.vy * dt;
                h.bumped_head = true;
            }
            self.vy = 0.0;
        } else if self.vy <= 0.0 && self.touching_below(tiles) {
            self.on_ground = true;
            self.jumping = false;
        }
        h
    }

    fn blocked(&self, t: &Tiles) -> bool {
        let e = 1e-4;
        let x0 = ((self.x - self.half_width + e) / t.size).floor() as i32;
        let x1 = ((self.x + self.half_width - e) / t.size).floor() as i32;
        let y0 = ((self.y + e) / t.size).floor() as i32;
        let y1 = ((self.y + self.height - e) / t.size).floor() as i32;
        (y0..=y1).any(|y| (x0..=x1).any(|x| t.solid(x, y)))
    }

    fn touching_below(&self, t: &Tiles) -> bool {
        let e = 1e-3;
        let y = ((self.y - e) / t.size).floor() as i32;
        let x0 = ((self.x - self.half_width + 1e-4) / t.size).floor() as i32;
        let x1 = ((self.x + self.half_width - 1e-4) / t.size).floor() as i32;
        (self.y / t.size).fract().abs() < 1e-3 && (x0..=x1).any(|x| t.solid(x, y))
    }
}
