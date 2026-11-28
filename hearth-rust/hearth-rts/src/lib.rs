//! Hearth RTS: the parts every real-time strategy game needs, for Hearth Engine's RTS starter kit.
//!
//! - `grid`: the map as cells (walls, mud, water).
//! - `flow`: flow fields, so hundreds of units share one path to a goal.
//! - `world`: units and buildings, orders (move, attack-move, attack, hold, gather, build), fighting, gathering,
//!   training, supply, and events for sounds and Hearth Script.
//! - `fog`: fog of war per team (seen now, seen before; walls block sight).
//! - `select`: drag-select, click-select, control groups.
//!
//! Steps run in a fixed order with no randomness, so a game that sends only orders between players stays the same
//! on every computer.

pub mod flow;
pub mod fog;
pub mod grid;
pub mod select;
pub mod world;

pub use grid::{Cell, Grid, Pos};
pub use world::{Attack, Event, Kind, Order, World};
