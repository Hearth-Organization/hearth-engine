//! Flow fields: one map of "which way to go" for a goal, shared by every unit heading there. Hundreds of units cost
//! about the same as one, which is what an RTS needs.

use std::cmp::Reverse;
use std::collections::{BinaryHeap, HashMap};

use crate::grid::{Cell, Grid, Pos};

const STRAIGHT: u32 = 10;
const DIAGONAL: u32 = 14;
pub const UNREACHABLE: u32 = u32::MAX;

/// Steps to the goal from every cell (`UNREACHABLE` where there's no way), and the best next cell from each.
#[derive(Clone, Debug)]
pub struct FlowField {
    pub goal: Cell,
    distance: Vec<u32>,
    next: Vec<Option<Cell>>,
    width: i32,
}

const DIRS: [(i32, i32, u32); 8] =
    [(1, 0, STRAIGHT), (-1, 0, STRAIGHT), (0, 1, STRAIGHT), (0, -1, STRAIGHT), (1, 1, DIAGONAL), (1, -1, DIAGONAL), (-1, 1, DIAGONAL), (-1, -1, DIAGONAL)];

/// A diagonal step may not cut a wall's corner.
fn can_step(grid: &Grid, from: Cell, dx: i32, dy: i32) -> bool {
    let to = Cell { x: from.x + dx, y: from.y + dy };
    grid.walkable(to) && (dx == 0 || dy == 0 || (grid.walkable(Cell { x: from.x + dx, y: from.y }) && grid.walkable(Cell { x: from.x, y: from.y + dy })))
}

impl FlowField {
    /// Works out the whole map's way to `goal` (the nearest walkable cell to it, if it's in a wall).
    pub fn new(grid: &Grid, goal: Cell) -> FlowField {
        let goal = grid.nearest_walkable(goal).unwrap_or(goal);
        let mut distance = vec![UNREACHABLE; grid.cells()];
        let mut heap = BinaryHeap::new();
        if let Some(i) = grid.index(goal) {
            if grid.walkable(goal) {
                distance[i] = 0;
                heap.push(Reverse((0u32, goal.y, goal.x)));
            }
        }
        while let Some(Reverse((d, y, x))) = heap.pop() {
            let c = Cell { x, y };
            let Some(ci) = grid.index(c) else { continue };
            if d > distance[ci] {
                continue;
            }
            for &(dx, dy, step) in &DIRS {
                // Walking from n to c: the cost is that of the cell being left.
                let n = Cell { x: x + dx, y: y + dy };
                if !can_step(grid, c, dx, dy) {
                    continue;
                }
                let Some(ni) = grid.index(n) else { continue };
                let nd = d + step * grid.cost(n) as u32;
                if nd < distance[ni] {
                    distance[ni] = nd;
                    heap.push(Reverse((nd, n.y, n.x)));
                }
            }
        }
        let mut next = vec![None; grid.cells()];
        for y in 0..grid.height {
            for x in 0..grid.width {
                let c = Cell { x, y };
                let ci = grid.index(c).unwrap_or(0);
                if distance[ci] == UNREACHABLE || c == goal {
                    continue;
                }
                let mut best: Option<(u32, Cell)> = None;
                for &(dx, dy, _) in &DIRS {
                    if !can_step(grid, c, dx, dy) {
                        continue;
                    }
                    let n = Cell { x: x + dx, y: y + dy };
                    let nd = grid.index(n).map(|i| distance[i]).unwrap_or(UNREACHABLE);
                    if nd < distance[ci] && best.map(|(bd, _)| nd < bd).unwrap_or(true) {
                        best = Some((nd, n));
                    }
                }
                next[ci] = best.map(|(_, n)| n);
            }
        }
        FlowField { goal, distance, next, width: grid.width }
    }

    fn i(&self, c: Cell) -> Option<usize> {
        let i = (c.y * self.width + c.x) as usize;
        (c.x >= 0 && c.y >= 0 && c.x < self.width && i < self.distance.len()).then_some(i)
    }

    pub fn reachable(&self, c: Cell) -> bool {
        self.i(c).map(|i| self.distance[i] != UNREACHABLE).unwrap_or(false)
    }

    pub fn distance(&self, c: Cell) -> u32 {
        self.i(c).map(|i| self.distance[i]).unwrap_or(UNREACHABLE)
    }

    pub fn next(&self, c: Cell) -> Option<Cell> {
        self.i(c).and_then(|i| self.next[i])
    }

    /// Which way to walk from a position (zero at the goal or where there's no way).
    pub fn direction(&self, grid: &Grid, p: Pos) -> Pos {
        let c = grid.cell_of(p);
        if c == self.goal {
            return grid.centre(c).minus(p).normalized();
        }
        match self.next(c) {
            Some(n) => grid.centre(n).minus(p).normalized(),
            None => Pos::default(),
        }
    }

    /// The cells from `from` to the goal (empty when there's no way).
    pub fn path(&self, from: Cell) -> Vec<Cell> {
        if !self.reachable(from) {
            return Vec::new();
        }
        let mut out = vec![from];
        let mut c = from;
        while c != self.goal {
            match self.next(c) {
                Some(n) => {
                    out.push(n);
                    c = n;
                }
                None => break,
            }
            if out.len() > self.distance.len() {
                break;
            }
        }
        out
    }
}

/// Keeps the fields for recent goals, so a group order costs one field and repeated orders cost nothing.
#[derive(Default)]
pub struct FlowCache {
    fields: HashMap<Cell, (FlowField, u64)>,
    clock: u64,
    pub keep: usize,
}

impl FlowCache {
    pub fn new(keep: usize) -> FlowCache {
        FlowCache { fields: HashMap::new(), clock: 0, keep: keep.max(1) }
    }

    pub fn get(&mut self, grid: &Grid, goal: Cell) -> &FlowField {
        self.clock += 1;
        let clock = self.clock;
        if !self.fields.contains_key(&goal) && self.fields.len() >= self.keep {
            if let Some(oldest) = self.fields.iter().min_by_key(|(_, (_, used))| *used).map(|(k, _)| *k) {
                self.fields.remove(&oldest);
            }
        }
        let entry = self.fields.entry(goal).or_insert_with(|| (FlowField::new(grid, goal), clock));
        entry.1 = clock;
        &entry.0
    }

    /// The map changed (a building went up): every field is worked out again when next asked for.
    pub fn clear(&mut self) {
        self.fields.clear();
    }

    pub fn len(&self) -> usize {
        self.fields.len()
    }

    pub fn is_empty(&self) -> bool {
        self.fields.is_empty()
    }
}
