//! Animation and prefabs, the parts other engines are loved for:
//!
//! - `states`: a character's animation states (idle, walk, run, attack) and the rules that move between them, with
//!   smooth blends (the idea from Fyrox's and Godot's animation state machines).
//! - `timeline`: animate anything (a door's angle, a light's colour, a camera) with keys and easing (Godot's
//!   AnimationPlayer idea).
//! - `prefab`: linked prefabs: change the original and every copy follows, while each copy keeps its own changes
//!   (Stride's and Unity's idea).

pub mod prefab;
pub mod states;
pub mod timeline;
