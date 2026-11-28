//! Wave Function Collapse (the idea is Maxim Gumin's, github.com/mxgmn/WaveFunctionCollapse, MIT; this is written
//! fresh in Rust): draw a small example map, and it builds new maps of any size in the same style. It learns which
//! tiles may sit next to which (left, right, above, below) and how common each is, then fills the new map one cell
//! at a time, always the most certain cell first, ruling out what can no longer fit around it.
//!
//! Example maps are rows of characters (`#` wall, `.` floor, `D` door, anything you like). Cells can be pinned
//! (`fixed`), so chunks of an endless world can match their neighbours' edges.

use std::collections::{BTreeMap, VecDeque};

use crate::noise::{hash, unit};

#[derive(Clone, Debug)]
pub struct Rules {
    /// The tiles, in the order they were first seen.
    pub tiles: Vec<char>,
    weight: Vec<f64>,
    /// allowed[d][a] = bit set of tiles that may be in direction d of tile a (0 right, 1 up, 2 left, 3 down).
    allowed: [Vec<Vec<bool>>; 4],
}

const DIRS: [(i32, i32); 4] = [(1, 0), (0, -1), (-1, 0), (0, 1)];

impl Rules {
    /// Learns from an example (rows of characters; `wrap`: the example's edges join, for tiling samples).
    pub fn learn(example: &str, wrap: bool) -> Result<Rules, String> {
        let rows: Vec<Vec<char>> = example.lines().map(|l| l.trim_end().chars().collect::<Vec<_>>()).filter(|l| !l.is_empty()).collect();
        if rows.is_empty() {
            return Err("the example is empty".into());
        }
        let w = rows[0].len();
        if rows.iter().any(|r| r.len() != w) {
            return Err("every row of the example must be the same length".into());
        }
        let h = rows.len();
        let mut index: BTreeMap<char, usize> = BTreeMap::new();
        let mut tiles = Vec::new();
        for r in &rows {
            for &c in r {
                index.entry(c).or_insert_with(|| {
                    tiles.push(c);
                    tiles.len() - 1
                });
            }
        }
        let n = tiles.len();
        let mut weight = vec![0f64; n];
        let mut allowed: [Vec<Vec<bool>>; 4] = std::array::from_fn(|_| vec![vec![false; n]; n]);
        for y in 0..h {
            for x in 0..w {
                let a = index[&rows[y][x]];
                weight[a] += 1.0;
                for (d, (dx, dy)) in DIRS.iter().enumerate() {
                    let (mut nx, mut ny) = (x as i32 + dx, y as i32 + dy);
                    if wrap {
                        nx = nx.rem_euclid(w as i32);
                        ny = ny.rem_euclid(h as i32);
                    } else if nx < 0 || ny < 0 || nx >= w as i32 || ny >= h as i32 {
                        continue;
                    }
                    let b = index[&rows[ny as usize][nx as usize]];
                    allowed[d][a][b] = true;
                }
            }
        }
        Ok(Rules { tiles, weight, allowed })
    }

    fn id(&self, c: char) -> Option<usize> {
        self.tiles.iter().position(|&t| t == c)
    }

    /// Whether `b` may be in direction `d` of `a` (what the example showed).
    pub fn may(&self, a: char, d: usize, b: char) -> bool {
        match (self.id(a), self.id(b)) {
            (Some(a), Some(b)) => self.allowed[d][a][b],
            _ => false,
        }
    }
}

/// Builds a `width` x `height` map. `fixed`: (x, y, tile) cells that must be that tile. Tries a few times from
/// different starts when it paints itself into a corner.
pub fn generate(rules: &Rules, width: usize, height: usize, seed: u64, fixed: &[(usize, usize, char)]) -> Result<Vec<Vec<char>>, String> {
    for attempt in 0..20u64 {
        if let Some(map) = attempt_once(rules, width, height, hash(seed, attempt as i64, 0, 0x57FC), fixed)? {
            return Ok(map);
        }
    }
    Err("couldn't build a map that fits the example's rules (try a bigger or more varied example, or fewer pinned cells)".into())
}

fn attempt_once(rules: &Rules, w: usize, h: usize, seed: u64, fixed: &[(usize, usize, char)]) -> Result<Option<Vec<Vec<char>>>, String> {
    let n = rules.tiles.len();
    let mut wave = vec![vec![true; n]; w * h];
    let mut queue: VecDeque<usize> = VecDeque::new();
    for &(x, y, c) in fixed {
        if x >= w || y >= h {
            return Err(format!("a pinned cell ({x}, {y}) is outside the {w} x {h} map"));
        }
        let t = rules.id(c).ok_or_else(|| format!("the pinned tile '{c}' isn't in the example"))?;
        let cell = &mut wave[y * w + x];
        for (i, v) in cell.iter_mut().enumerate() {
            *v = *v && i == t;
        }
        queue.push_back(y * w + x);
    }
    if !propagate(rules, &mut wave, w, h, &mut queue) {
        return Ok(None);
    }
    let mut step = 0u64;
    loop {
        // The least certain cell still open, with a tiny random tie-break.
        let mut best: Option<(f64, usize)> = None;
        for (i, cell) in wave.iter().enumerate() {
            let options: Vec<usize> = (0..n).filter(|&t| cell[t]).collect();
            if options.is_empty() {
                return Ok(None);
            }
            if options.len() == 1 {
                continue;
            }
            let total: f64 = options.iter().map(|&t| rules.weight[t]).sum();
            let entropy = total.ln() - options.iter().map(|&t| rules.weight[t] * rules.weight[t].ln()).sum::<f64>() / total;
            let e = entropy + unit(hash(seed, i as i64, step as i64, 1)) as f64 * 1e-6;
            if best.map(|(b, _)| e < b).unwrap_or(true) {
                best = Some((e, i));
            }
        }
        let Some((_, cell)) = best else { break };
        // Collapse it, choosing by how common each tile was in the example.
        let options: Vec<usize> = (0..n).filter(|&t| wave[cell][t]).collect();
        let total: f64 = options.iter().map(|&t| rules.weight[t]).sum();
        let mut r = unit(hash(seed, cell as i64, step as i64, 2)) as f64 * total;
        let mut pick = options[options.len() - 1];
        for &t in &options {
            r -= rules.weight[t];
            if r <= 0.0 {
                pick = t;
                break;
            }
        }
        for (t, v) in wave[cell].iter_mut().enumerate() {
            *v = t == pick;
        }
        queue.push_back(cell);
        if !propagate(rules, &mut wave, w, h, &mut queue) {
            return Ok(None);
        }
        step += 1;
    }
    Ok(Some((0..h).map(|y| (0..w).map(|x| rules.tiles[wave[y * w + x].iter().position(|&v| v).unwrap_or(0)]).collect()).collect()))
}

/// Rules out, cell by cell, tiles that no longer have any possible neighbour. False: a cell has nothing left.
fn propagate(rules: &Rules, wave: &mut [Vec<bool>], w: usize, h: usize, queue: &mut VecDeque<usize>) -> bool {
    let n = rules.tiles.len();
    while let Some(i) = queue.pop_front() {
        let (x, y) = ((i % w) as i32, (i / w) as i32);
        for (d, (dx, dy)) in DIRS.iter().enumerate() {
            let (nx, ny) = (x + dx, y + dy);
            if nx < 0 || ny < 0 || nx >= w as i32 || ny >= h as i32 {
                continue;
            }
            let j = (ny as usize) * w + nx as usize;
            let mut changed = false;
            for b in 0..n {
                if !wave[j][b] {
                    continue;
                }
                let supported = (0..n).any(|a| wave[i][a] && rules.allowed[d][a][b]);
                if !supported {
                    wave[j][b] = false;
                    changed = true;
                }
            }
            if changed {
                if !wave[j].iter().any(|&v| v) {
                    return false;
                }
                queue.push_back(j);
            }
        }
    }
    true
}
