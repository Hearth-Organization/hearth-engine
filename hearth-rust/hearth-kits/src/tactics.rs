//! Turn-based battles on a grid (like XCOM or Fire Emblem): turns by speed, where a unit can walk with its moves,
//! and who it can hit.

use std::cmp::Reverse;
use std::collections::{BinaryHeap, HashMap};

use hearth_rts::{Cell, Grid};

#[derive(Clone, Debug, PartialEq)]
pub struct Fighter {
    pub name: String,
    pub team: u8,
    pub at: Cell,
    pub health: i32,
    /// Cells of ground (cost 1 each; mud costs more) it can walk in a turn.
    pub moves: u32,
    /// Higher goes first.
    pub speed: i32,
    pub range: u32,
    pub damage: i32,
}

pub struct Battle {
    pub grid: Grid,
    pub fighters: Vec<Fighter>,
    order: Vec<usize>,
    turn: usize,
    pub round: u32,
    moved: bool,
    acted: bool,
}

impl Battle {
    pub fn new(grid: Grid, fighters: Vec<Fighter>) -> Battle {
        let mut b = Battle { grid, fighters, order: Vec::new(), turn: 0, round: 1, moved: false, acted: false };
        b.plan_round();
        b
    }

    fn plan_round(&mut self) {
        let mut order: Vec<usize> = (0..self.fighters.len()).filter(|&i| self.fighters[i].health > 0).collect();
        order.sort_by_key(|&i| (Reverse(self.fighters[i].speed), i));
        self.order = order;
        self.turn = 0;
    }

    /// Whose turn it is.
    pub fn current(&self) -> Option<usize> {
        self.order.get(self.turn).copied()
    }

    fn occupied(&self, c: Cell, except: usize) -> bool {
        self.fighters.iter().enumerate().any(|(i, f)| i != except && f.health > 0 && f.at == c)
    }

    /// Every cell the fighter can walk to this turn, with what it costs (no diagonals; others block the way).
    pub fn reachable(&self, who: usize) -> HashMap<Cell, u32> {
        let f = &self.fighters[who];
        let mut best: HashMap<Cell, u32> = HashMap::new();
        let mut heap = BinaryHeap::new();
        best.insert(f.at, 0);
        heap.push(Reverse((0u32, f.at)));
        while let Some(Reverse((d, c))) = heap.pop() {
            if best.get(&c).is_some_and(|&b| d > b) {
                continue;
            }
            for (dx, dy) in [(1, 0), (-1, 0), (0, 1), (0, -1)] {
                let n = Cell { x: c.x + dx, y: c.y + dy };
                if !self.grid.walkable(n) || self.occupied(n, who) {
                    continue;
                }
                let nd = d + self.grid.cost(n) as u32;
                if nd <= f.moves && best.get(&n).is_none_or(|&b| nd < b) {
                    best.insert(n, nd);
                    heap.push(Reverse((nd, n)));
                }
            }
        }
        best
    }

    fn distance(a: Cell, b: Cell) -> u32 {
        a.x.abs_diff(b.x) + a.y.abs_diff(b.y)
    }

    /// Enemies in range of where the fighter stands.
    pub fn targets(&self, who: usize) -> Vec<usize> {
        let f = &self.fighters[who];
        (0..self.fighters.len()).filter(|&i| self.fighters[i].team != f.team && self.fighters[i].health > 0 && Battle::distance(f.at, self.fighters[i].at) <= f.range).collect()
    }

    pub fn walk(&mut self, to: Cell) -> Result<(), String> {
        let who = self.current().ok_or("the battle is over")?;
        if self.moved {
            return Err("this fighter has already moved this turn".into());
        }
        if !self.reachable(who).contains_key(&to) {
            return Err("that's too far, or the way is blocked".into());
        }
        self.fighters[who].at = to;
        self.moved = true;
        Ok(())
    }

    pub fn attack(&mut self, target: usize) -> Result<i32, String> {
        let who = self.current().ok_or("the battle is over")?;
        if self.acted {
            return Err("this fighter has already attacked this turn".into());
        }
        if !self.targets(who).contains(&target) {
            return Err("that one is out of range".into());
        }
        let dmg = self.fighters[who].damage;
        self.fighters[target].health -= dmg;
        self.acted = true;
        Ok(dmg)
    }

    /// Ends the turn; a new round starts after everyone has gone. Returns the winning team once one is left.
    pub fn end_turn(&mut self) -> Option<u8> {
        self.moved = false;
        self.acted = false;
        self.turn += 1;
        while let Some(i) = self.current() {
            if self.fighters[i].health > 0 {
                break;
            }
            self.turn += 1;
        }
        if self.current().is_none() {
            self.round += 1;
            self.plan_round();
        }
        self.winner()
    }

    pub fn winner(&self) -> Option<u8> {
        let mut teams = self.fighters.iter().filter(|f| f.health > 0).map(|f| f.team);
        let first = teams.next()?;
        teams.all(|t| t == first).then_some(first)
    }
}
