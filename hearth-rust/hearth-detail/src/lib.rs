//! Hearth Detail: Nanite-style detail for Hearth Engine.
//!
//! A model is cut into pieces of up to 128 triangles, and groups of neighbouring pieces get simpler versions, level
//! after level (`build`). The game then draws, piece by piece, the simplest version whose error is too small to see
//! from where the camera is: full detail up close, far fewer triangles far away, and never a crack between pieces.
//! Hearth Engine's renderer does that choice on the graphics card each frame from the `.hdetail` file (`format`).

pub mod build;
pub mod cluster;
pub mod format;
pub mod load;
pub mod math;
pub mod mesh;
pub mod simplify;

pub use build::{build, Cluster, Detail};
pub use mesh::{Mesh, Vertex};
