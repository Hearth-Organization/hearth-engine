//! The RTS world: units and buildings of each team, the resources on the map, and what happens each step: moving
//! (with flow fields and without bumping into each other), fighting, gathering, building and training.
//!
//! Everything runs in id order with no randomness, so the same orders give the same game on every computer (what
//! multiplayer that sends only orders needs).

use std::collections::{HashMap, VecDeque};

use crate::flow::FlowCache;
use crate::fog::Fog;
use crate::grid::{Cell, Grid, Pos, BLOCKED};

pub type Team = u8;
pub type UnitId = u32;
pub type ResourceId = u32;

#[derive(Clone, Debug, PartialEq)]
pub struct Attack {
    /// Metres from edge to edge.
    pub range: f32,
    pub damage: f32,
    /// Seconds between hits.
    pub every: f32,
}

/// A kind of unit or building ("Worker", "Knight", "Barracks"), made by the game or its starter kit.
#[derive(Clone, Debug, PartialEq)]
pub struct Kind {
    pub name: String,
    /// Metres a second (0 for buildings).
    pub speed: f32,
    pub radius: f32,
    pub health: f32,
    pub sight: f32,
    pub attack: Option<Attack>,
    /// How much a worker carries back per trip (0: can't gather), and how fast it gathers (a second).
    pub carry: f32,
    pub gather_rate: f32,
    /// What it costs, by resource name, and the seconds to train or build it.
    pub cost: Vec<(String, u32)>,
    pub time: f32,
    /// Supply it uses (units) or gives (buildings).
    pub supply: u32,
    /// Buildings: the cells it covers, what it trains, and whether workers bring resources to it.
    pub footprint: (i32, i32),
    pub trains: Vec<String>,
    pub drop_off: bool,
    /// Workers: the buildings they can put up.
    pub builds: Vec<String>,
}

impl Kind {
    pub fn unit(name: &str, speed: f32, radius: f32, health: f32) -> Kind {
        Kind {
            name: name.into(),
            speed,
            radius,
            health,
            sight: 8.0,
            attack: None,
            carry: 0.0,
            gather_rate: 0.0,
            cost: Vec::new(),
            time: 1.0,
            supply: 1,
            footprint: (0, 0),
            trains: Vec::new(),
            drop_off: false,
            builds: Vec::new(),
        }
    }

    pub fn building(name: &str, w: i32, h: i32, health: f32) -> Kind {
        Kind { footprint: (w, h), supply: 0, speed: 0.0, sight: 6.0, ..Kind::unit(name, 0.0, w.max(h) as f32 * 0.5, health) }
    }

    pub fn is_building(&self) -> bool {
        self.footprint.0 > 0
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Order {
    Move(Pos),
    /// Move, but stop to fight anything on the way.
    AttackMove(Pos),
    Attack(UnitId),
    /// Stay put; fight what comes in range.
    Hold,
    Gather(ResourceId),
    Build { kind: String, at: Cell },
}

#[derive(Clone, Debug)]
pub struct Unit {
    pub id: UnitId,
    pub kind: usize,
    pub team: Team,
    pub pos: Pos,
    pub velocity: Pos,
    pub health: f32,
    pub orders: VecDeque<Order>,
    cooldown: f32,
    /// What a worker carries: (resource name, amount).
    pub carrying: Option<(String, f32)>,
    /// Buildings: seconds of building left (None when finished), what it's training, and where new units go.
    pub unfinished: Option<f32>,
    pub training: VecDeque<(usize, f32)>,
    pub rally: Option<Pos>,
    /// The goal cells it takes up (buildings).
    cells: Vec<Cell>,
}

#[derive(Clone, Debug)]
pub struct Resource {
    pub id: ResourceId,
    pub name: String,
    pub pos: Pos,
    pub amount: f32,
}

#[derive(Clone, Debug, Default)]
pub struct Player {
    pub stock: HashMap<String, u32>,
    pub supply_used: u32,
    pub supply_cap: u32,
}

/// What happened in a step: the game plays sounds and effects for these, and Hearth Script `when`s run on them.
#[derive(Clone, Debug, PartialEq)]
pub enum Event {
    Hit { by: UnitId, target: UnitId, damage: f32 },
    Died { id: UnitId, team: Team, kind: String },
    Trained { id: UnitId, team: Team, kind: String },
    Built { id: UnitId, team: Team, kind: String },
    Brought { by: UnitId, team: Team, resource: String, amount: u32 },
    Arrived { id: UnitId },
    /// An order couldn't be done (not enough of a resource, no room to build, no supply): why, for the player.
    Cant { team: Team, why: String },
}

#[derive(Clone)]
pub struct World {
    pub grid: Grid,
    pub kinds: Vec<Kind>,
    pub units: Vec<Unit>,
    pub resources: Vec<Resource>,
    pub players: HashMap<Team, Player>,
    pub fog: HashMap<Team, Fog>,
    pub events: Vec<Event>,
    pub time: f32,
    flows: FlowCache,
    next_id: u32,
}

impl World {
    pub fn new(grid: Grid, kinds: Vec<Kind>) -> World {
        World { grid, kinds, units: Vec::new(), resources: Vec::new(), players: HashMap::new(), fog: HashMap::new(), events: Vec::new(), time: 0.0, flows: FlowCache::new(64), next_id: 1 }
    }

    pub fn kind_named(&self, name: &str) -> Option<usize> {
        self.kinds.iter().position(|k| k.name == name)
    }

    pub fn unit(&self, id: UnitId) -> Option<&Unit> {
        self.units.binary_search_by_key(&id, |u| u.id).ok().map(|i| &self.units[i])
    }

    fn unit_index(&self, id: UnitId) -> Option<usize> {
        self.units.binary_search_by_key(&id, |u| u.id).ok()
    }

    pub fn player(&mut self, team: Team) -> &mut Player {
        self.players.entry(team).or_default()
    }

    pub fn add_resource(&mut self, name: &str, pos: Pos, amount: f32) -> ResourceId {
        let id = self.next_id;
        self.next_id += 1;
        self.resources.push(Resource { id, name: name.into(), pos, amount });
        id
    }

    fn footprint_cells(&self, kind: usize, at: Cell) -> Vec<Cell> {
        let (w, h) = self.kinds[kind].footprint;
        let mut out = Vec::new();
        for y in 0..h {
            for x in 0..w {
                out.push(Cell { x: at.x - w / 2 + x, y: at.y - h / 2 + y });
            }
        }
        out
    }

    /// Whether a building fits there: every cell walkable, and no unit standing in the way.
    pub fn fits(&self, kind: usize, at: Cell) -> bool {
        let cells = self.footprint_cells(kind, at);
        cells.iter().all(|&c| self.grid.walkable(c))
            && !self.units.iter().any(|u| !self.kinds[u.kind].is_building() && cells.contains(&self.grid.cell_of(u.pos)))
    }

    /// Puts a unit or a finished building on the map. Buildings fill their cells (units walk around them).
    pub fn spawn(&mut self, kind: usize, team: Team, pos: Pos) -> UnitId {
        let id = self.next_id;
        self.next_id += 1;
        let k = &self.kinds[kind];
        let mut cells = Vec::new();
        let mut pos = pos;
        if k.is_building() {
            let at = self.grid.cell_of(pos);
            cells = self.footprint_cells(kind, at);
            for &c in &cells {
                self.grid.set(c, BLOCKED);
            }
            self.flows.clear();
            pos = self.cells_middle(&cells);
            self.player(team).supply_cap += self.kinds[kind].supply;
        } else {
            self.player(team).supply_used += self.kinds[kind].supply;
        }
        let k = &self.kinds[kind];
        self.units.push(Unit {
            id,
            kind,
            team,
            pos,
            velocity: Pos::default(),
            health: k.health,
            orders: VecDeque::new(),
            cooldown: 0.0,
            carrying: None,
            unfinished: None,
            training: VecDeque::new(),
            rally: None,
            cells,
        });
        id
    }

    fn cells_middle(&self, cells: &[Cell]) -> Pos {
        let n = cells.len().max(1) as f32;
        let s = cells.iter().fold(Pos::default(), |s, &c| s.plus(self.grid.centre(c)));
        s.scale(1.0 / n)
    }

    fn can_afford(&self, team: Team, kind: usize) -> Result<(), String> {
        let empty = Player::default();
        let p = self.players.get(&team).unwrap_or(&empty);
        for (res, n) in &self.kinds[kind].cost {
            let have = p.stock.get(res).copied().unwrap_or(0);
            if have < *n {
                return Err(format!("not enough {res} for a {}: it needs {n}, you have {have}", self.kinds[kind].name));
            }
        }
        Ok(())
    }

    fn pay(&mut self, team: Team, kind: usize) {
        let cost = self.kinds[kind].cost.clone();
        let p = self.player(team);
        for (res, n) in cost {
            let s = p.stock.entry(res).or_default();
            *s = s.saturating_sub(n);
        }
    }

    /// Gives an order. `queue` adds it after the ones already given (shift in most RTS games); otherwise it replaces
    /// them. Several units ordered to one place spread out around it instead of fighting for one spot.
    pub fn order(&mut self, ids: &[UnitId], order: Order, queue: bool) {
        let movers: Vec<usize> = ids.iter().filter_map(|&id| self.unit_index(id)).filter(|&i| !self.kinds[self.units[i].kind].is_building()).collect();
        let spots = match &order {
            Order::Move(p) | Order::AttackMove(p) => self.spread(&movers, *p),
            _ => Vec::new(),
        };
        for (k, &i) in movers.iter().enumerate() {
            let o = match &order {
                Order::Move(_) => Order::Move(spots[k]),
                Order::AttackMove(_) => Order::AttackMove(spots[k]),
                other => other.clone(),
            };
            let u = &mut self.units[i];
            if !queue {
                u.orders.clear();
            }
            u.orders.push_back(o);
        }
        // Buildings take a move order as where to send what they train.
        if let Order::Move(p) = order {
            for &id in ids {
                if let Some(i) = self.unit_index(id) {
                    if self.kinds[self.units[i].kind].is_building() {
                        self.units[i].rally = Some(p);
                    }
                }
            }
        }
    }

    /// Spots in a block around `to`, nearest first, each unit taking the nearest one left (so the group keeps its shape
    /// and nobody crosses the others).
    fn spread(&self, movers: &[usize], to: Pos) -> Vec<Pos> {
        if movers.len() <= 1 {
            return vec![to; movers.len()];
        }
        let gap = movers.iter().map(|&i| self.kinds[self.units[i].kind].radius).fold(0.0, f32::max) * 2.2;
        let side = (movers.len() as f32).sqrt().ceil() as i32;
        let mut spots: Vec<Pos> = Vec::new();
        let mut r: i32 = 0;
        while spots.len() < movers.len() * 2 && r < 64 {
            for dy in -r..=r {
                for dx in -r..=r {
                    if dx.abs().max(dy.abs()) != r {
                        continue;
                    }
                    let p = to.plus(Pos::new(dx as f32 * gap, dy as f32 * gap));
                    if self.grid.walkable(self.grid.cell_of(p)) {
                        spots.push(p);
                    }
                }
            }
            r += 1;
            if r > side + 2 && spots.len() >= movers.len() {
                break;
            }
        }
        if spots.is_empty() {
            return vec![to; movers.len()];
        }
        let middle = movers.iter().fold(Pos::default(), |s, &i| s.plus(self.units[i].pos)).scale(1.0 / movers.len() as f32);
        let mut taken = vec![false; spots.len()];
        let mut out = vec![to; movers.len()];
        // Units at the front of the group (nearest the goal) choose first.
        let mut order: Vec<usize> = (0..movers.len()).collect();
        order.sort_by(|&a, &b| self.units[movers[a]].pos.dist(to).total_cmp(&self.units[movers[b]].pos.dist(to)).then(a.cmp(&b)));
        for k in order {
            let offset = self.units[movers[k]].pos.minus(middle);
            let want = to.plus(offset);
            let best = (0..spots.len()).filter(|&s| !taken[s]).min_by(|&a, &b| spots[a].dist(want).total_cmp(&spots[b].dist(want)).then(a.cmp(&b)));
            if let Some(s) = best {
                taken[s] = true;
                out[k] = spots[s];
            }
        }
        out
    }

    /// Asks a building to train a unit: paid now, ready after the kind's time.
    pub fn train(&mut self, building: UnitId, kind_name: &str) -> Result<(), String> {
        let i = self.unit_index(building).ok_or("that building is gone")?;
        let b = &self.units[i];
        let bk = &self.kinds[b.kind];
        if !bk.trains.iter().any(|t| t == kind_name) {
            return Err(format!("a {} can't train a {kind_name}", bk.name));
        }
        if b.unfinished.is_some() {
            return Err(format!("the {} isn't finished yet", bk.name));
        }
        let kind = self.kind_named(kind_name).ok_or_else(|| format!("there's no kind called {kind_name}"))?;
        let team = b.team;
        self.can_afford(team, kind)?;
        self.pay(team, kind);
        let t = self.kinds[kind].time;
        self.units[i].training.push_back((kind, t));
        Ok(())
    }

    /// Steps the world forward `dt` seconds (call it at a fixed rate, like 20 times a second, for the same game on
    /// every computer).
    pub fn step(&mut self, dt: f32) {
        self.time += dt;
        let n = self.units.len();
        let wanted: Vec<Pos> = (0..n).map(|i| self.think(i, dt)).collect();
        self.produce(dt);
        self.move_units(&wanted, dt);
        self.bury();
        self.look();
    }

    fn enemy_near(&self, i: usize, within: f32) -> Option<usize> {
        let u = &self.units[i];
        let fog = self.fog.get(&u.team);
        self.units
            .iter()
            .enumerate()
            .filter(|(_, o)| o.team != u.team && o.health > 0.0)
            .filter(|(_, o)| fog.map(|f| f.sees(&self.grid, o.pos)).unwrap_or(true))
            .map(|(j, o)| (j, o.pos.dist(u.pos) - self.kinds[o.kind].radius - self.kinds[u.kind].radius))
            .filter(|&(_, d)| d <= within)
            .min_by(|a, b| a.1.total_cmp(&b.1).then(a.0.cmp(&b.0)))
            .map(|(j, _)| j)
    }

    /// Hits `target` if it's in range (returns true), else returns where to walk.
    fn fight(&mut self, i: usize, target: usize) -> Result<(), Pos> {
        let (u, t) = (&self.units[i], &self.units[target]);
        let Some(a) = self.kinds[u.kind].attack.clone() else { return Err(u.pos) };
        let gap = t.pos.dist(u.pos) - self.kinds[t.kind].radius - self.kinds[u.kind].radius;
        if gap > a.range {
            return Err(t.pos);
        }
        let (by, tid) = (u.id, t.id);
        let u = &mut self.units[i];
        if u.cooldown <= 0.0 {
            u.cooldown = a.every;
            self.units[target].health -= a.damage;
            self.events.push(Event::Hit { by, target: tid, damage: a.damage });
        }
        Ok(())
    }

    fn walk_to(&mut self, i: usize, to: Pos) -> Pos {
        let u = &self.units[i];
        let speed = self.kinds[u.kind].speed;
        let here = self.grid.cell_of(u.pos);
        let goal = self.grid.cell_of(to);
        if here == goal || u.pos.dist(to) < self.grid.cell_size {
            return to.minus(u.pos).normalized().scale(speed.min(u.pos.dist(to) * 4.0));
        }
        let pos = u.pos;
        let field = self.flows.get(&self.grid, goal);
        field.direction(&self.grid, pos).scale(speed)
    }

    /// What unit `i` does this step: returns the velocity it wants.
    fn think(&mut self, i: usize, dt: f32) -> Pos {
        let kind = &self.kinds[self.units[i].kind];
        if kind.is_building() || self.units[i].health <= 0.0 {
            return Pos::default();
        }
        let (radius, attack_range, sight) = (kind.radius, kind.attack.as_ref().map(|a| a.range), kind.sight);
        if self.units[i].cooldown > 0.0 {
            self.units[i].cooldown = (self.units[i].cooldown - dt).max(0.0);
        }
        let Some(order) = self.units[i].orders.front().cloned() else {
            // Idle: fight anything that comes in range.
            if let Some(r) = attack_range {
                if let Some(t) = self.enemy_near(i, r) {
                    let _ = self.fight(i, t);
                }
            }
            return Pos::default();
        };
        match order {
            Order::Move(to) => {
                if self.units[i].pos.dist(to) <= radius.max(0.25) {
                    self.finish_order(i);
                    return Pos::default();
                }
                self.walk_to(i, to)
            }
            Order::AttackMove(to) => {
                if let Some(t) = self.enemy_near(i, sight) {
                    return match self.fight(i, t) {
                        Ok(()) => Pos::default(),
                        Err(go) => self.walk_to(i, go),
                    };
                }
                if self.units[i].pos.dist(to) <= radius.max(0.25) {
                    self.finish_order(i);
                    return Pos::default();
                }
                self.walk_to(i, to)
            }
            Order::Attack(id) => match self.unit_index(id).filter(|&t| self.units[t].health > 0.0) {
                None => {
                    self.finish_order(i);
                    Pos::default()
                }
                Some(t) => match self.fight(i, t) {
                    Ok(()) => Pos::default(),
                    Err(go) => self.walk_to(i, go),
                },
            },
            Order::Hold => {
                if let Some(r) = attack_range {
                    if let Some(t) = self.enemy_near(i, r) {
                        let _ = self.fight(i, t);
                    }
                }
                Pos::default()
            }
            Order::Gather(rid) => self.gather(i, rid, dt),
            Order::Build { kind, at } => self.build(i, &kind, at, dt),
        }
    }

    fn finish_order(&mut self, i: usize) {
        let u = &mut self.units[i];
        u.orders.pop_front();
        if u.orders.is_empty() {
            self.events.push(Event::Arrived { id: u.id });
        }
    }

    fn gather(&mut self, i: usize, rid: ResourceId, dt: f32) -> Pos {
        let k = self.kinds[self.units[i].kind].clone();
        if k.carry <= 0.0 {
            self.finish_order(i);
            return Pos::default();
        }
        let team = self.units[i].team;
        let carried = self.units[i].carrying.as_ref().map(|c| c.1).unwrap_or(0.0);
        let res = self.resources.iter().position(|r| r.id == rid);
        let full = carried >= k.carry;
        let empty = res.map(|r| self.resources[r].amount <= 0.0).unwrap_or(true);
        if full || (empty && carried > 0.0) {
            // Take it home: the nearest finished drop-off of this team.
            let pos = self.units[i].pos;
            let home = self
                .units
                .iter()
                .filter(|b| b.team == team && b.unfinished.is_none() && self.kinds[b.kind].drop_off)
                .min_by(|a, b| a.pos.dist(pos).total_cmp(&b.pos.dist(pos)).then(a.id.cmp(&b.id)))
                .map(|b| (b.pos, self.kinds[b.kind].radius));
            let Some((home, home_r)) = home else {
                self.events.push(Event::Cant { team, why: format!("there's nowhere to bring the {} to", self.units[i].carrying.as_ref().map(|c| c.0.as_str()).unwrap_or("load")) });
                self.finish_order(i);
                return Pos::default();
            };
            if pos.dist(home) <= home_r + k.radius + self.grid.cell_size {
                let (name, amount) = self.units[i].carrying.take().unwrap_or_default();
                let n = amount.round() as u32;
                *self.player(team).stock.entry(name.clone()).or_default() += n;
                self.events.push(Event::Brought { by: self.units[i].id, team, resource: name, amount: n });
                if empty {
                    self.finish_order(i);
                }
                return Pos::default();
            }
            return self.walk_to(i, home);
        }
        let Some(r) = res.filter(|_| !empty) else {
            self.finish_order(i);
            return Pos::default();
        };
        let rpos = self.resources[r].pos;
        if self.units[i].pos.dist(rpos) > k.radius + self.grid.cell_size {
            return self.walk_to(i, rpos);
        }
        let take = (k.gather_rate * dt).min(self.resources[r].amount).min(k.carry - carried);
        self.resources[r].amount -= take;
        let name = self.resources[r].name.clone();
        let c = self.units[i].carrying.get_or_insert((name, 0.0));
        c.1 += take;
        Pos::default()
    }

    fn build(&mut self, i: usize, kind_name: &str, at: Cell, dt: f32) -> Pos {
        let team = self.units[i].team;
        let Some(kind) = self.kind_named(kind_name) else {
            self.events.push(Event::Cant { team, why: format!("there's no building called {kind_name}") });
            self.finish_order(i);
            return Pos::default();
        };
        if !self.kinds[self.units[i].kind].builds.iter().any(|b| b == kind_name) {
            self.events.push(Event::Cant { team, why: format!("a {} can't build a {kind_name}", self.kinds[self.units[i].kind].name) });
            self.finish_order(i);
            return Pos::default();
        }
        let cells = self.footprint_cells(kind, at);
        // Already started here? Keep building it.
        let site = self.units.iter().position(|b| b.team == team && b.kind == kind && b.cells == cells);
        let middle = self.cells_middle(&cells);
        let reach = self.kinds[kind].radius + self.kinds[self.units[i].kind].radius + self.grid.cell_size;
        if self.units[i].pos.dist(middle) > reach {
            // Walk to the nearest open cell beside it.
            let target = cells
                .iter()
                .flat_map(|c| [(1, 0), (-1, 0), (0, 1), (0, -1)].map(|(dx, dy)| Cell { x: c.x + dx, y: c.y + dy }))
                .filter(|c| !cells.contains(c) && self.grid.walkable(*c))
                .min_by(|a, b| self.grid.centre(*a).dist(self.units[i].pos).total_cmp(&self.grid.centre(*b).dist(self.units[i].pos)).then(a.cmp(b)))
                .map(|c| self.grid.centre(c))
                .unwrap_or(middle);
            // Standing beside it is close enough, even if it's big.
            if self.units[i].pos.dist(target) >= self.grid.cell_size * 0.5 {
                return self.walk_to(i, target);
            }
        }
        match site {
            Some(s) => {
                if let Some(left) = self.units[s].unfinished {
                    let left = left - dt;
                    if left <= 0.0 {
                        self.units[s].unfinished = None;
                        self.events.push(Event::Built { id: self.units[s].id, team, kind: kind_name.into() });
                        self.finish_order(i);
                    } else {
                        self.units[s].unfinished = Some(left);
                        let k = &self.kinds[kind];
                        self.units[s].health = (self.units[s].health + k.health * dt / k.time.max(0.01)).min(k.health);
                    }
                } else {
                    self.finish_order(i);
                }
            }
            None => {
                if !self.fits(kind, at) {
                    self.events.push(Event::Cant { team, why: format!("there's no room for a {kind_name} there") });
                    self.finish_order(i);
                    return Pos::default();
                }
                if let Err(why) = self.can_afford(team, kind) {
                    self.events.push(Event::Cant { team, why });
                    self.finish_order(i);
                    return Pos::default();
                }
                self.pay(team, kind);
                let id = self.spawn(kind, team, self.grid.centre(at));
                let s = self.unit_index(id).unwrap_or(0);
                self.units[s].unfinished = Some(self.kinds[kind].time);
                self.units[s].health = self.kinds[kind].health * 0.1;
            }
        }
        Pos::default()
    }

    fn produce(&mut self, dt: f32) {
        let ids: Vec<UnitId> = self.units.iter().map(|u| u.id).collect();
        for id in ids {
            let Some(b) = self.unit_index(id) else { continue };
            if self.units[b].unfinished.is_some() || self.units[b].health <= 0.0 {
                continue;
            }
            let Some(&(kind, left)) = self.units[b].training.front() else { continue };
            let team = self.units[b].team;
            let left = left - dt;
            if left > 0.0 {
                self.units[b].training[0].1 = left;
                continue;
            }
            let need = self.kinds[kind].supply;
            let p = self.player(team);
            if p.supply_used + need > p.supply_cap {
                self.units[b].training[0].1 = 0.0;
                if !self.events.iter().any(|e| matches!(e, Event::Cant { team: t, .. } if *t == team)) {
                    self.events.push(Event::Cant { team, why: "you need more supply: build another house".into() });
                }
                continue;
            }
            self.units[b].training.pop_front();
            let cells = self.units[b].cells.clone();
            let rally = self.units[b].rally;
            let exit = cells
                .iter()
                .flat_map(|c| [(0, 1), (1, 0), (-1, 0), (0, -1)].map(|(dx, dy)| Cell { x: c.x + dx, y: c.y + dy }))
                .find(|c| !cells.contains(c) && self.grid.walkable(*c))
                .map(|c| self.grid.centre(c))
                .unwrap_or(self.units[b].pos);
            let id = self.spawn(kind, team, exit);
            self.events.push(Event::Trained { id, team, kind: self.kinds[kind].name.clone() });
            if let Some(r) = rally {
                self.order(&[id], Order::Move(r), false);
            }
        }
    }

    fn move_units(&mut self, wanted: &[Pos], dt: f32) {
        // Who's near whom, by cell (so pushing apart doesn't compare every unit with every other).
        let mut near: HashMap<Cell, Vec<usize>> = HashMap::new();
        for (i, u) in self.units.iter().enumerate() {
            if !self.kinds[u.kind].is_building() {
                near.entry(self.grid.cell_of(u.pos)).or_default().push(i);
            }
        }
        let mut new_pos: Vec<Pos> = self.units.iter().map(|u| u.pos).collect();
        for (i, u) in self.units.iter().enumerate() {
            let k = &self.kinds[u.kind];
            if k.is_building() || u.health <= 0.0 {
                continue;
            }
            let mut push = Pos::default();
            let c = self.grid.cell_of(u.pos);
            for dy in -1..=1 {
                for dx in -1..=1 {
                    for &j in near.get(&Cell { x: c.x + dx, y: c.y + dy }).map(Vec::as_slice).unwrap_or(&[]) {
                        if j == i {
                            continue;
                        }
                        let o = &self.units[j];
                        let gap = k.radius + self.kinds[o.kind].radius;
                        let d = u.pos.dist(o.pos);
                        if d < gap {
                            let away = if d > 1e-4 { u.pos.minus(o.pos).scale(1.0 / d) } else { Pos::new(if i < j { -1.0 } else { 1.0 }, 0.0) };
                            push = push.plus(away.scale((gap - d) * 0.5));
                        }
                    }
                }
            }
            let v = wanted.get(i).copied().unwrap_or_default();
            let step = v.scale(dt).plus(push);
            new_pos[i] = self.slide(u.pos, step);
        }
        for (u, (p, v)) in self.units.iter_mut().zip(new_pos.into_iter().zip(wanted.iter())) {
            if dt > 0.0 {
                u.velocity = *v;
            }
            u.pos = p;
        }
    }

    /// Moves without entering a wall: slides along it instead.
    fn slide(&self, from: Pos, step: Pos) -> Pos {
        let ok = |p: Pos| self.grid.walkable(self.grid.cell_of(p));
        let to = from.plus(step);
        if ok(to) {
            return to;
        }
        let x = from.plus(Pos::new(step.x, 0.0));
        if ok(x) {
            return x;
        }
        let y = from.plus(Pos::new(0.0, step.y));
        if ok(y) {
            return y;
        }
        from
    }

    fn bury(&mut self) {
        let mut freed = false;
        let mut dead = Vec::new();
        for u in &self.units {
            if u.health <= 0.0 {
                dead.push((u.id, u.team, u.kind, u.cells.clone()));
            }
        }
        for (id, team, kind, cells) in dead {
            self.events.push(Event::Died { id, team, kind: self.kinds[kind].name.clone() });
            let supply = self.kinds[kind].supply;
            let building = self.kinds[kind].is_building();
            let p = self.player(team);
            if building {
                p.supply_cap = p.supply_cap.saturating_sub(supply);
            } else {
                p.supply_used = p.supply_used.saturating_sub(supply);
            }
            for c in cells {
                self.grid.set(c, 1);
                freed = true;
            }
        }
        self.units.retain(|u| u.health > 0.0);
        if freed {
            self.flows.clear();
        }
    }

    fn look(&mut self) {
        let teams: Vec<Team> = self.players.keys().copied().collect();
        for team in teams {
            let fog = self.fog.entry(team).or_insert_with(|| Fog::new(&self.grid));
            fog.clear_now();
            for u in self.units.iter().filter(|u| u.team == team) {
                fog.reveal(&self.grid, u.pos, self.kinds[u.kind].sight);
            }
        }
    }

    /// The fields worked out and kept (for tests and the debug view).
    pub fn flow_fields_kept(&self) -> usize {
        self.flows.len()
    }

    /// The events since the last call.
    pub fn take_events(&mut self) -> Vec<Event> {
        std::mem::take(&mut self.events)
    }
}
