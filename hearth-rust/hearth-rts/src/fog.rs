//! Fog of war, per team: what's seen right now, and what's been seen before (shown dimmed). Walls block sight.

use crate::grid::{Cell, Grid, Pos};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Sight {
    Never,
    Before,
    Now,
}

#[derive(Clone, Debug)]
pub struct Fog {
    width: i32,
    height: i32,
    seen_before: Vec<bool>,
    /// How many units see each cell right now.
    seeing: Vec<u16>,
}

impl Fog {
    pub fn new(grid: &Grid) -> Fog {
        let n = grid.cells();
        Fog { width: grid.width, height: grid.height, seen_before: vec![false; n], seeing: vec![0; n] }
    }

    fn i(&self, c: Cell) -> Option<usize> {
        (c.x >= 0 && c.y >= 0 && c.x < self.width && c.y < self.height).then_some((c.y * self.width + c.x) as usize)
    }

    /// Starts a new look: nothing is seen now (what was seen stays remembered).
    pub fn clear_now(&mut self) {
        self.seeing.fill(0);
    }

    /// A unit at `at` sees `range` metres around, unless a wall is in the way.
    pub fn reveal(&mut self, grid: &Grid, at: Pos, range: f32) {
        let from = grid.cell_of(at);
        let r = (range / grid.cell_size).ceil() as i32;
        for dy in -r..=r {
            for dx in -r..=r {
                let c = Cell { x: from.x + dx, y: from.y + dy };
                let Some(i) = self.i(c) else { continue };
                if grid.centre(c).dist(at) > range + grid.cell_size * 0.5 || !line_of_sight(grid, from, c) {
                    continue;
                }
                self.seeing[i] = self.seeing[i].saturating_add(1);
                self.seen_before[i] = true;
            }
        }
    }

    pub fn sight(&self, c: Cell) -> Sight {
        match self.i(c) {
            Some(i) if self.seeing[i] > 0 => Sight::Now,
            Some(i) if self.seen_before[i] => Sight::Before,
            _ => Sight::Never,
        }
    }

    pub fn sees(&self, grid: &Grid, p: Pos) -> bool {
        self.sight(grid.cell_of(p)) == Sight::Now
    }
}

/// Whether nothing blocks the view between two cells (a wall can be seen, but not through).
pub fn line_of_sight(grid: &Grid, a: Cell, b: Cell) -> bool {
    let (mut x, mut y) = (a.x, a.y);
    let (dx, dy) = ((b.x - a.x).abs(), -(b.y - a.y).abs());
    let (sx, sy) = (if a.x < b.x { 1 } else { -1 }, if a.y < b.y { 1 } else { -1 });
    let mut err = dx + dy;
    while (x, y) != (b.x, b.y) {
        if (x, y) != (a.x, a.y) && !grid.walkable(Cell { x, y }) {
            return false;
        }
        let e2 = 2 * err;
        if e2 >= dy {
            err += dy;
            x += sx;
        }
        if e2 <= dx {
            err += dx;
            y += sy;
        }
    }
    true
}
