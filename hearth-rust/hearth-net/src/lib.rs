//! Rollback co-op for Hearth Engine games (GGRS, MIT OR Apache-2.0): only the players' inputs travel over the
//! network; every computer runs the whole game. When a remote input arrives late, the game goes back to the last
//! sure moment and runs forward again with it, in the same frame, so nobody waits. This needs a game that gives
//! the same result from the same inputs every time, which the RTS kit does.
//!
//! An input is the player's latest command with a number (`seq`): the same number again means "nothing new", so
//! GGRS guessing "the same as last time" for a late player never repeats an order.

use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::net::SocketAddr;

use ggrs::{Config, GgrsRequest, InputStatus, PredictRepeatLast};
use hearth_rts::{Order, Pos, World};
use serde::{Deserialize, Serialize};

/// What a player did this frame. Positions are in centimetres so they travel as whole numbers.
#[derive(Copy, Clone, PartialEq, Eq, Default, Debug, Serialize, Deserialize)]
pub struct Command {
    /// Goes up by one for each new command; 0 = nothing yet.
    pub seq: u16,
    /// 0 nothing, 1 move, 2 attack-move, 3 hold.
    pub kind: u8,
    pub x_cm: i32,
    pub y_cm: i32,
}

impl Command {
    pub fn order(seq: u16, kind: u8, at: Pos) -> Command {
        Command { seq, kind, x_cm: (at.x * 100.0).round() as i32, y_cm: (at.y * 100.0).round() as i32 }
    }
    fn pos(&self) -> Pos {
        Pos::new(self.x_cm as f32 / 100.0, self.y_cm as f32 / 100.0)
    }
}

#[derive(Debug)]
pub struct RtsConfig;

impl Config for RtsConfig {
    type Input = Command;
    type InputPredictor = PredictRepeatLast;
    type State = Game;
    type Address = SocketAddr;
}

/// The whole game state that rolls back: the world and the last command each player has had carried out.
#[derive(Clone)]
pub struct Game {
    pub world: World,
    pub frame: i32,
    applied: Vec<u16>,
    /// Steps a second (fixed, the same on every computer).
    pub rate: f32,
}

impl Game {
    pub fn new(world: World, players: usize) -> Game {
        Game { world, frame: 0, applied: vec![0; players], rate: 20.0 }
    }

    /// One frame with every player's input. Player n commands team n.
    pub fn advance(&mut self, inputs: &[(Command, InputStatus)]) {
        for (player, (cmd, status)) in inputs.iter().enumerate() {
            if *status == InputStatus::Disconnected || cmd.seq == 0 || self.applied.get(player) == Some(&cmd.seq) {
                continue;
            }
            if let Some(a) = self.applied.get_mut(player) {
                *a = cmd.seq;
            }
            let ids: Vec<u32> = self.world.units.iter().filter(|u| u.team as usize == player && !self.world.kinds[u.kind].is_building()).map(|u| u.id).collect();
            let order = match cmd.kind {
                1 => Order::Move(cmd.pos()),
                2 => Order::AttackMove(cmd.pos()),
                3 => Order::Hold,
                _ => continue,
            };
            self.world.order(&ids, order, false);
        }
        self.world.step(1.0 / self.rate);
        self.world.take_events();
        self.frame += 1;
    }

    /// A fingerprint of everything that matters: two computers with the same one are in the same game.
    pub fn checksum(&self) -> u128 {
        let mut h = DefaultHasher::new();
        self.frame.hash(&mut h);
        self.applied.hash(&mut h);
        for u in &self.world.units {
            (u.id, u.team, u.pos.x.to_bits(), u.pos.y.to_bits(), u.health.to_bits(), u.orders.len()).hash(&mut h);
        }
        let mut teams: Vec<_> = self.world.players.iter().collect();
        teams.sort_by_key(|(t, _)| **t);
        for (t, p) in teams {
            let mut stock: Vec<_> = p.stock.iter().collect();
            stock.sort();
            (t, stock, p.supply_used, p.supply_cap).hash(&mut h);
        }
        h.finish() as u128
    }

    /// Does what GGRS asks: save, load, or advance.
    pub fn handle(&mut self, requests: Vec<GgrsRequest<RtsConfig>>) {
        for r in requests {
            match r {
                GgrsRequest::SaveGameState { cell, frame } => {
                    debug_assert_eq!(frame, self.frame);
                    cell.save(frame, Some(self.clone()), Some(self.checksum()));
                }
                GgrsRequest::LoadGameState { cell, .. } => {
                    if let Some(g) = cell.load() {
                        *self = g;
                    }
                }
                GgrsRequest::AdvanceFrame { inputs } => self.advance(&inputs),
            }
        }
    }
}
