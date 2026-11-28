//! Hearth's acoustics: what walls do to a sound, and how a space echoes. Pure Rust, worked out with physics rays.

use glam::Vec3;
use hearth_physics::Physics;
use hearth_world::Entity;

#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    /// Each wall cuts this many decibels...
    pub db_per_wall: f32,
    /// ...and lowers the brightest sound that gets through (hertz) by this share.
    pub muffle_per_wall: f32,
    pub max_walls: u32,
}

impl Default for Settings {
    fn default() -> Settings {
        Settings { db_per_wall: 7.0, muffle_per_wall: 0.18, max_walls: 4 }
    }
}

/// How a space echoes.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Echo {
    /// 0 = no echo, near 1 = a long echo.
    pub feedback: f32,
    /// How loud the echo is (decibels).
    pub send_db: f32,
    /// The average distance to the surroundings (metres; infinite outdoors).
    pub size: f32,
    /// The share of directions that hit something (1 = closed in).
    pub closed: f32,
}

impl Settings {
    /// What `walls` walls do: (the brightest sound that gets through in hertz, the change in loudness in decibels).
    pub fn muffle(&self, walls: u32) -> (f32, f32) {
        let w = walls.min(self.max_walls) as f32;
        (20_000.0 * self.muffle_per_wall.powf(w), -self.db_per_wall * w)
    }

    /// Measures the space around the listener with rays (`ray(direction)` gives the distance to what it hits).
    pub fn room(&self, ray: impl Fn(Vec3) -> Option<f32>) -> Echo {
        let mut dirs = Vec::new();
        for k in 0..16 {
            let a = k as f32 / 16.0 * std::f32::consts::TAU;
            dirs.push(Vec3::new(a.cos(), a.sin(), 0.0));
            dirs.push(Vec3::new(a.cos() * 0.7, a.sin() * 0.7, 0.7).normalize());
        }
        dirs.push(Vec3::Z);
        dirs.push(Vec3::NEG_Z);
        let hits: Vec<f32> = dirs.iter().filter_map(|&d| ray(d)).collect();
        let closed = hits.len() as f32 / dirs.len() as f32;
        if hits.is_empty() {
            return Echo { feedback: 0.0, send_db: -60.0, size: f32::INFINITY, closed: 0.0 };
        }
        let size = hits.iter().sum::<f32>() / hits.len() as f32;
        // Bigger and more closed in: longer and louder echoes.
        let feedback = (0.35 + 0.6 * (size / 30.0).min(1.0)) * closed;
        let send_db = -30.0 + 22.0 * closed * (size / 20.0).min(1.0);
        Echo { feedback: feedback.min(0.95), send_db, size, closed }
    }
}

/// How many solid things are between two points (sensors and the two entities themselves don't count).
pub fn walls_between(p: &Physics, from: Vec3, to: Vec3, ignore_a: Option<Entity>, ignore_b: Option<Entity>) -> u32 {
    let dir = to - from;
    let total = dir.length();
    if total < 1e-3 {
        return 0;
    }
    let d = dir / total;
    let mut skip: Vec<Entity> = ignore_a.into_iter().chain(ignore_b).collect();
    let mut walls = 0;
    // Each ray passes through the walls already counted, so each wall is counted once however thick it is.
    while walls < 16 {
        let Some((hit, _)) = p.ray_past(from, d, total, &skip) else { break };
        walls += 1;
        skip.push(hit);
    }
    walls
}
