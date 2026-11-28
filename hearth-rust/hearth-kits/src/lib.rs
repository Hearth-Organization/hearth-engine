//! Hearth Kits: the parts each kind of game needs, so a new game starts from a working base instead of nothing.
//! The RTS kit is its own crate (`hearth-rts`); these are the others.
//!
//! - `platformer`: running and jumping that feels right (coyote time, jump buffering, short and long jumps).
//! - `dialogue`: conversations with choices, written as plain text, that remember what was said.
//! - `quests`: objectives that count what happens in the game, and rewards.
//! - `inventory`: slots and stacks, and crafting from recipes (survival and RPG games).
//! - `tactics`: turn-based battles on a grid: who moves when, where a unit can reach, who it can hit.
//! - `towers`: tower defense: waves, enemies that find their way to the base, towers that aim and fire.

pub mod dialogue;
pub mod inventory;
pub mod platformer;
pub mod quests;
pub mod tactics;
pub mod towers;
