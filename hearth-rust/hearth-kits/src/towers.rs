//! Tower defense: waves of enemies walk to the base along the best way (a flow field, so placing a tower changes
//! their path), towers aim at the enemy nearest the base in range, kills give gold, enemies that get through cost
//! lives. Building a tower is refused if it would close off the way completely.

use hearth_rts::flow::FlowField;
use hearth_rts::{Cell, Grid, Pos};

#[derive(Clone, Debug, PartialEq)]
pub struct EnemyKind {
    pub name: String,
    pub health: f32,
    pub speed: f32,
    pub gold: u32,
}

#[derive(Clone, Debug, PartialEq)]
pub struct TowerKind {
    pub name: String,
    pub cost: u32,
    pub range: f32,
    pub damage: f32,
    pub every: f32,
}

#[derive(Clone, Debug)]
pub struct Enemy {
    pub kind: usize,
    pub pos: Pos,
    pub health: f32,
}

#[derive(Clone, Debug)]
pub struct Tower {
    pub kind: usize,
    pub at: Cell,
    cooldown: f32,
}

/// One wave: `count` of an enemy kind, one every `gap` seconds, starting `delay` seconds after the wave before.
#[derive(Clone, Debug)]
pub struct Wave {
    pub enemy: usize,
    pub count: u32,
    pub gap: f32,
    pub delay: f32,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Happened {
    Spawned(usize),
    Shot { tower: usize, enemy: usize },
    Killed { gold: u32 },
    GotThrough,
    WaveStarted(usize),
    Won,
    Lost,
}

pub struct Defense {
    pub grid: Grid,
    pub start: Cell,
    pub base: Cell,
    pub enemy_kinds: Vec<EnemyKind>,
    pub tower_kinds: Vec<TowerKind>,
    pub waves: Vec<Wave>,
    pub enemies: Vec<Enemy>,
    pub towers: Vec<Tower>,
    pub gold: u32,
    pub lives: u32,
    flow: FlowField,
    wave: usize,
    sent: u32,
    clock: f32,
    over: bool,
}

impl Defense {
    pub fn new(grid: Grid, start: Cell, base: Cell, enemy_kinds: Vec<EnemyKind>, tower_kinds: Vec<TowerKind>, waves: Vec<Wave>) -> Defense {
        let flow = FlowField::new(&grid, base);
        let clock = -waves.first().map(|w| w.delay).unwrap_or(0.0);
        Defense { grid, start, base, enemy_kinds, tower_kinds, waves, enemies: Vec::new(), towers: Vec::new(), gold: 100, lives: 20, flow, wave: 0, sent: 0, clock, over: false }
    }

    /// Builds a tower, unless it costs too much, the cell is taken, or it would leave no way to the base.
    pub fn build(&mut self, kind: usize, at: Cell) -> Result<(), String> {
        let k = self.tower_kinds.get(kind).ok_or("there's no tower like that")?;
        if self.gold < k.cost {
            return Err(format!("a {} costs {} gold; you have {}", k.name, k.cost, self.gold));
        }
        if !self.grid.walkable(at) || at == self.start || at == self.base {
            return Err("you can't build there".into());
        }
        if self.enemies.iter().any(|e| self.grid.cell_of(e.pos) == at) {
            return Err("an enemy is standing there".into());
        }
        let cost = self.grid.cost(at);
        self.grid.set(at, hearth_rts::grid::BLOCKED);
        let flow = FlowField::new(&self.grid, self.base);
        let stuck = !flow.reachable(self.start) || self.enemies.iter().any(|e| !flow.reachable(self.grid.cell_of(e.pos)));
        if stuck {
            self.grid.set(at, cost);
            return Err("that would block the way to the base completely".into());
        }
        self.flow = flow;
        self.gold -= self.tower_kinds[kind].cost;
        self.towers.push(Tower { kind, at, cooldown: 0.0 });
        Ok(())
    }

    pub fn step(&mut self, dt: f32) -> Vec<Happened> {
        let mut out = Vec::new();
        if self.over {
            return out;
        }
        // Sending the wave.
        self.clock += dt;
        if let Some(w) = self.waves.get(self.wave).cloned() {
            if self.sent == 0 && self.clock >= 0.0 {
                out.push(Happened::WaveStarted(self.wave));
            }
            while self.sent < w.count && self.clock >= self.sent as f32 * w.gap {
                let k = &self.enemy_kinds[w.enemy];
                self.enemies.push(Enemy { kind: w.enemy, pos: self.grid.centre(self.start), health: k.health });
                out.push(Happened::Spawned(self.enemies.len() - 1));
                self.sent += 1;
            }
            if self.sent >= w.count && self.enemies.is_empty() {
                self.wave += 1;
                self.sent = 0;
                self.clock = -self.waves.get(self.wave).map(|w| w.delay).unwrap_or(0.0);
            }
        }
        // Walking.
        let base = self.grid.centre(self.base);
        for e in &mut self.enemies {
            let speed = self.enemy_kinds[e.kind].speed;
            let here = self.grid.cell_of(e.pos);
            let dir = if here == self.base { base.minus(e.pos).normalized() } else { self.flow.direction(&self.grid, e.pos) };
            e.pos = e.pos.plus(dir.scale(speed * dt));
        }
        let before = self.enemies.len();
        self.enemies.retain(|e| e.pos.dist(base) > 0.3);
        for _ in self.enemies.len()..before {
            self.lives = self.lives.saturating_sub(1);
            out.push(Happened::GotThrough);
        }
        // Shooting: each tower aims at the enemy in range that is nearest the base.
        for (t, tower) in self.towers.iter_mut().enumerate() {
            let k = &self.tower_kinds[tower.kind];
            tower.cooldown = (tower.cooldown - dt).max(0.0);
            if tower.cooldown > 0.0 {
                continue;
            }
            let from = self.grid.centre(tower.at);
            let target = self
                .enemies
                .iter()
                .enumerate()
                .filter(|(_, e)| e.health > 0.0 && e.pos.dist(from) <= k.range)
                .min_by(|a, b| {
                    let da = self.flow.distance(self.grid.cell_of(a.1.pos));
                    let db = self.flow.distance(self.grid.cell_of(b.1.pos));
                    da.cmp(&db).then(a.0.cmp(&b.0))
                })
                .map(|(i, _)| i);
            if let Some(i) = target {
                self.enemies[i].health -= k.damage;
                tower.cooldown = k.every;
                out.push(Happened::Shot { tower: t, enemy: i });
            }
        }
        let kinds = &self.enemy_kinds;
        let mut gold = 0;
        self.enemies.retain(|e| {
            if e.health <= 0.0 {
                gold += kinds[e.kind].gold;
                false
            } else {
                true
            }
        });
        if gold > 0 {
            self.gold += gold;
            out.push(Happened::Killed { gold });
        }
        if self.lives == 0 {
            self.over = true;
            out.push(Happened::Lost);
        } else if self.wave >= self.waves.len() && self.enemies.is_empty() {
            self.over = true;
            out.push(Happened::Won);
        }
        out
    }

    pub fn is_over(&self) -> bool {
        self.over
    }
}
