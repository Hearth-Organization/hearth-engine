//! The Level Generator's core: an endless world of rooms and halls, built chunk by chunk from a seed.
//!
//! - **The same everywhere**: a chunk comes only from (seed, chunk x, chunk y), so every player gets the same world
//!   and a chunk can be built again at any time (when the player comes back) without being saved.
//! - **Always connected**: inside a chunk the walls are a maze that reaches every cell (then some are knocked out,
//!   and some chunks get a big room); the lines between chunks are decided by the line itself, with at least two
//!   doorways on every side. So the whole endless world is one connected place, and nobody is ever walled in.
//! - **No wall built twice**: each chunk owns its south and west border lines; the north and east ones belong to
//!   the neighbours.
//! - Things to place (lights, props, pickups) come from the same seed per cell, and smooth "zones" (dark areas,
//!   flooded areas) come from value noise, so they stretch across chunks naturally.
//!
//! The game's own levels (like Noclip Protocol's Level 0) build their look on top of this.

pub mod noise;
pub mod wfc;

use noise::{hash, unit, zone};

#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    pub seed: u64,
    /// Cells per chunk side, and metres per cell.
    pub chunk_cells: i32,
    pub cell_size: f32,
    /// Share of the maze's leftover inside walls knocked out (0 = a pure maze, 1 = open plan).
    pub open: f32,
    /// Chance of each extra doorway on a chunk border, beyond the two every side gets.
    pub border_open: f32,
    /// Chance a chunk gets one big room.
    pub room_chance: f32,
}

impl Default for Settings {
    fn default() -> Settings {
        Settings { seed: 1, chunk_cells: 6, cell_size: 4.0, open: 0.2, border_open: 0.15, room_chance: 0.35 }
    }
}

/// A wall on one cell edge, in world cell coordinates: from (x, y) one cell east (`along_x`) or one cell north.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Wall {
    pub x: i32,
    pub y: i32,
    pub along_x: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Room {
    pub x: i32,
    pub y: i32,
    pub w: i32,
    pub h: i32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Chunk {
    pub cx: i32,
    pub cy: i32,
    /// The walls this chunk builds: its inside walls and its south and west border lines.
    pub walls: Vec<Wall>,
    /// Doorways on its south and west borders (cell edges left open).
    pub doorways: Vec<Wall>,
    pub room: Option<Room>,
}

const SALT_MAZE: u64 = 1;
const SALT_KNOCK: u64 = 2;
const SALT_ROOM: u64 = 3;
const SALT_BORDER: u64 = 4;

/// Which edges of a border line (`n` cells long) are doorways: two different ones always, more by chance.
fn border_doors(s: &Settings, line: (i32, i32, bool)) -> Vec<bool> {
    let n = s.chunk_cells.max(1) as usize;
    let (lx, ly, horizontal) = line;
    let h = |k: u64| hash(s.seed, lx as i64, ly as i64, SALT_BORDER * 16 + k + if horizontal { 0 } else { 8 });
    let mut open = vec![false; n];
    let a = (h(0) % n as u64) as usize;
    open[a] = true;
    if n > 1 {
        let b = (a + 1 + (h(1) % (n as u64 - 1)) as usize) % n;
        open[b] = true;
    }
    for (i, o) in open.iter_mut().enumerate() {
        if unit(hash(s.seed, lx as i64 * 31 + i as i64, ly as i64, SALT_BORDER * 16 + 5 + if horizontal { 0 } else { 8 })) < s.border_open {
            *o = true;
        }
    }
    open
}

/// Builds one chunk.
pub fn chunk(s: &Settings, cx: i32, cy: i32) -> Chunk {
    let n = s.chunk_cells.max(1);
    let (ox, oy) = (cx * n, cy * n);
    let idx = |x: i32, y: i32| (y * n + x) as usize;

    // Inside walls: start with every one, carve a maze that reaches every cell (a random depth-first walk).
    // east[i]: wall on the east edge of cell i (inside only); north[i]: on its north edge.
    let mut east = vec![true; (n * n) as usize];
    let mut north = vec![true; (n * n) as usize];
    let mut seen = vec![false; (n * n) as usize];
    let mut stack = vec![(0i32, 0i32)];
    seen[0] = true;
    let mut step = 0u64;
    while let Some(&(x, y)) = stack.last() {
        let mut options: Vec<(i32, i32)> = [(1, 0), (-1, 0), (0, 1), (0, -1)]
            .iter()
            .map(|&(dx, dy)| (x + dx, y + dy))
            .filter(|&(nx, ny)| nx >= 0 && ny >= 0 && nx < n && ny < n && !seen[idx(nx, ny)])
            .collect();
        if options.is_empty() {
            stack.pop();
            continue;
        }
        step += 1;
        let pick = (hash(s.seed, cx as i64, cy as i64, SALT_MAZE * 1_000_003 + step) % options.len() as u64) as usize;
        let (nx, ny) = options.swap_remove(pick);
        if nx > x {
            east[idx(x, y)] = false;
        } else if nx < x {
            east[idx(nx, ny)] = false;
        } else if ny > y {
            north[idx(x, y)] = false;
        } else {
            north[idx(nx, ny)] = false;
        }
        seen[idx(nx, ny)] = true;
        stack.push((nx, ny));
    }
    // Knock some leftover walls out, so it isn't a pure maze.
    for (i, (e, no)) in east.iter_mut().zip(north.iter_mut()).enumerate() {
        if *e && unit(hash(s.seed, (ox as i64) * 977 + i as i64, oy as i64, SALT_KNOCK)) < s.open {
            *e = false;
        }
        if *no && unit(hash(s.seed, (ox as i64) * 977 + i as i64, oy as i64, SALT_KNOCK + 100)) < s.open {
            *no = false;
        }
    }
    // A big room: no walls inside a rectangle.
    let mut room = None;
    if n >= 3 && unit(hash(s.seed, cx as i64, cy as i64, SALT_ROOM)) < s.room_chance {
        let w = 2 + (hash(s.seed, cx as i64, cy as i64, SALT_ROOM + 1) % (n as u64 - 1).max(1)) as i32;
        let h = 2 + (hash(s.seed, cx as i64, cy as i64, SALT_ROOM + 2) % (n as u64 - 1).max(1)) as i32;
        let (w, h) = (w.min(n), h.min(n));
        let rx = (hash(s.seed, cx as i64, cy as i64, SALT_ROOM + 3) % (n - w + 1) as u64) as i32;
        let ry = (hash(s.seed, cx as i64, cy as i64, SALT_ROOM + 4) % (n - h + 1) as u64) as i32;
        for y in ry..ry + h {
            for x in rx..rx + w {
                if x + 1 < rx + w {
                    east[idx(x, y)] = false;
                }
                if y + 1 < ry + h {
                    north[idx(x, y)] = false;
                }
            }
        }
        room = Some(Room { x: ox + rx, y: oy + ry, w, h });
    }

    let mut walls = Vec::new();
    for y in 0..n {
        for x in 0..n {
            // The chunk's own east and north border edges belong to the neighbours.
            if x + 1 < n && east[idx(x, y)] {
                walls.push(Wall { x: ox + x + 1, y: oy + y, along_x: false });
            }
            if y + 1 < n && north[idx(x, y)] {
                walls.push(Wall { x: ox + x, y: oy + y + 1, along_x: true });
            }
        }
    }
    let mut doorways = Vec::new();
    let south = border_doors(s, (cx, cy, true));
    let west = border_doors(s, (cx, cy, false));
    for i in 0..n {
        let sw = Wall { x: ox + i, y: oy, along_x: true };
        let ww = Wall { x: ox, y: oy + i, along_x: false };
        if south[i as usize] { doorways.push(sw) } else { walls.push(sw) }
        if west[i as usize] { doorways.push(ww) } else { walls.push(ww) }
    }
    walls.sort();
    Chunk { cx, cy, walls, doorways, room }
}

fn chunk_of(s: &Settings, cell: i32) -> i32 {
    cell.div_euclid(s.chunk_cells.max(1))
}

/// Whether there's a wall between cell (x, y) and the cell next to it in direction (dx, dy) (one step along an axis).
/// Works anywhere in the endless world.
pub fn blocked(s: &Settings, x: i32, y: i32, dx: i32, dy: i32) -> bool {
    // The edge's owner: the chunk on its north or east side.
    let wall = match (dx, dy) {
        (1, 0) => Wall { x: x + 1, y, along_x: false },
        (-1, 0) => Wall { x, y, along_x: false },
        (0, 1) => Wall { x, y: y + 1, along_x: true },
        (0, -1) => Wall { x, y, along_x: true },
        _ => return true,
    };
    let c = chunk(s, chunk_of(s, wall.x), chunk_of(s, wall.y));
    c.walls.binary_search(&wall).is_ok()
}

/// A wall as a box in metres (Z up, floor at 0): centre x, y and size x, y.
pub fn wall_box(s: &Settings, w: &Wall, thickness: f32) -> [f32; 4] {
    let c = s.cell_size;
    if w.along_x {
        [(w.x as f32 + 0.5) * c, w.y as f32 * c, c + thickness, thickness]
    } else {
        [w.x as f32 * c, (w.y as f32 + 0.5) * c, thickness, c + thickness]
    }
}

/// A deterministic choice per cell: "does this cell get a light / a prop / a pickup" for a named kind of thing.
pub fn cell_chance(s: &Settings, x: i32, y: i32, kind: &str, chance: f32) -> bool {
    let k = kind.bytes().fold(0xcbf29ce484222325u64, |h, b| (h ^ b as u64).wrapping_mul(0x100000001b3));
    unit(hash(s.seed, x as i64, y as i64, k)) < chance
}

/// A smooth 0-1 value over the world (for dark zones and the like), `scale` cells across.
pub fn zone_value(s: &Settings, x: f32, y: f32, scale: f32, kind: u64) -> f32 {
    zone(s.seed ^ kind.wrapping_mul(0x9E3779B97F4A7C15), x / scale.max(0.01), y / scale.max(0.01))
}
