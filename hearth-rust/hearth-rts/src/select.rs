//! Choosing units: drag a box, click one, and keep control groups (Ctrl+1 to make, 1 to pick again).

use crate::grid::Pos;
use crate::world::{Team, UnitId, World};

#[derive(Clone, Debug, Default)]
pub struct Selection {
    pub chosen: Vec<UnitId>,
    groups: [Vec<UnitId>; 10],
}

impl Selection {
    /// Every unit of `team` inside the box between two corners on the ground. Units win over buildings: a box over
    /// soldiers and a barracks picks just the soldiers.
    pub fn drag(&mut self, world: &World, team: Team, a: Pos, b: Pos) {
        let (lo, hi) = (Pos::new(a.x.min(b.x), a.y.min(b.y)), Pos::new(a.x.max(b.x), a.y.max(b.y)));
        let inside = |p: Pos, r: f32| p.x + r >= lo.x && p.x - r <= hi.x && p.y + r >= lo.y && p.y - r <= hi.y;
        let found: Vec<(UnitId, bool)> = world
            .units
            .iter()
            .filter(|u| u.team == team && inside(u.pos, world.kinds[u.kind].radius))
            .map(|u| (u.id, world.kinds[u.kind].is_building()))
            .collect();
        let units: Vec<UnitId> = found.iter().filter(|f| !f.1).map(|f| f.0).collect();
        self.chosen = if units.is_empty() { found.iter().map(|f| f.0).collect() } else { units };
    }

    /// The unit of `team` under a click (the nearest whose body is under it).
    pub fn click(&mut self, world: &World, team: Team, at: Pos, add: bool) {
        let hit = world
            .units
            .iter()
            .filter(|u| u.team == team && u.pos.dist(at) <= world.kinds[u.kind].radius.max(0.3))
            .min_by(|a, b| a.pos.dist(at).total_cmp(&b.pos.dist(at)).then(a.id.cmp(&b.id)))
            .map(|u| u.id);
        match (hit, add) {
            (Some(id), true) => {
                if let Some(k) = self.chosen.iter().position(|&c| c == id) {
                    self.chosen.remove(k);
                } else {
                    self.chosen.push(id);
                }
            }
            (Some(id), false) => self.chosen = vec![id],
            (None, false) => self.chosen.clear(),
            (None, true) => {}
        }
    }

    pub fn set_group(&mut self, n: usize) {
        if let Some(g) = self.groups.get_mut(n) {
            *g = self.chosen.clone();
        }
    }

    /// Picks a control group again (without the ones that died).
    pub fn recall(&mut self, world: &World, n: usize) {
        if let Some(g) = self.groups.get_mut(n) {
            g.retain(|&id| world.unit(id).is_some());
            self.chosen = g.clone();
        }
    }

    /// Forgets units that died.
    pub fn tidy(&mut self, world: &World) {
        self.chosen.retain(|&id| world.unit(id).is_some());
    }
}
