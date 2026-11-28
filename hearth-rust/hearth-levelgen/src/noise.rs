//! Hashing and smooth noise that give the same answer on every computer (no floating-point randomness).

/// A 64-bit hash of a seed, a place and a salt (SplitMix64 steps).
pub fn hash(seed: u64, x: i64, y: i64, salt: u64) -> u64 {
    let mut z = seed ^ (x as u64).wrapping_mul(0x9E3779B97F4A7C15) ^ (y as u64).wrapping_mul(0xC2B2AE3D27D4EB4F) ^ salt.wrapping_mul(0x165667B19E3779F9);
    for _ in 0..2 {
        z = z.wrapping_add(0x9E3779B97F4A7C15);
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58476D1CE4E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D049BB133111EB);
        z ^= z >> 31;
    }
    z
}

/// 0 to 1 (never 1).
pub fn unit(h: u64) -> f32 {
    (h >> 40) as f32 / (1u64 << 24) as f32
}

/// Smooth value noise, 0 to 1: random at whole numbers, eased in between.
pub fn zone(seed: u64, x: f32, y: f32) -> f32 {
    let (x0, y0) = (x.floor(), y.floor());
    let (fx, fy) = (x - x0, y - y0);
    let ease = |t: f32| t * t * (3.0 - 2.0 * t);
    let at = |i: f32, j: f32| unit(hash(seed, i as i64, j as i64, 77));
    let (a, b, c, d) = (at(x0, y0), at(x0 + 1.0, y0), at(x0, y0 + 1.0), at(x0 + 1.0, y0 + 1.0));
    let (u, v) = (ease(fx), ease(fy));
    let top = a + (b - a) * u;
    let bottom = c + (d - c) * u;
    top + (bottom - top) * v
}
