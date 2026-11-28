//! Simple shapes for tests, placeholders and the editor (Z is up).

use crate::{MeshData, Vertex};

pub fn sphere(rings: u32, segments: u32) -> MeshData {
    let (rings, segments) = (rings.max(2), segments.max(3));
    let mut m = MeshData::default();
    for r in 0..=rings {
        let v = r as f32 / rings as f32;
        let theta = v * std::f32::consts::PI;
        for s in 0..=segments {
            let u = s as f32 / segments as f32;
            let phi = u * std::f32::consts::TAU;
            let n = [theta.sin() * phi.cos(), theta.sin() * phi.sin(), theta.cos()];
            m.vertices.push(Vertex { pos: n, normal: n, uv: [u, v] });
        }
    }
    let row = segments + 1;
    for r in 0..rings {
        for s in 0..segments {
            let (a, b) = (r * row + s, (r + 1) * row + s);
            // Counter-clockwise from outside.
            m.indices.extend([a, b, a + 1, a + 1, b, b + 1]);
        }
    }
    m
}

/// A flat square, `size` wide, facing up.
pub fn plane(size: f32) -> MeshData {
    let h = size / 2.0;
    let n = [0.0, 0.0, 1.0];
    MeshData {
        vertices: vec![
            Vertex { pos: [-h, -h, 0.0], normal: n, uv: [0.0, 0.0] },
            Vertex { pos: [h, -h, 0.0], normal: n, uv: [1.0, 0.0] },
            Vertex { pos: [h, h, 0.0], normal: n, uv: [1.0, 1.0] },
            Vertex { pos: [-h, h, 0.0], normal: n, uv: [0.0, 1.0] },
        ],
        indices: vec![0, 1, 2, 0, 2, 3],
    }
}

/// A box 1 unit on each side, centred.
pub fn cube() -> MeshData {
    let mut m = MeshData::default();
    let faces: [([f32; 3], [f32; 3], [f32; 3]); 6] = [
        ([1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]),
        ([-1.0, 0.0, 0.0], [0.0, -1.0, 0.0], [0.0, 0.0, 1.0]),
        ([0.0, 1.0, 0.0], [-1.0, 0.0, 0.0], [0.0, 0.0, 1.0]),
        ([0.0, -1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, 1.0]),
        ([0.0, 0.0, 1.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]),
        ([0.0, 0.0, -1.0], [-1.0, 0.0, 0.0], [0.0, 1.0, 0.0]),
    ];
    for (n, u, v) in faces {
        let base = m.vertices.len() as u32;
        for (a, b) in [(-1.0f32, -1.0f32), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)] {
            let p = [0.5 * (n[0] + a * u[0] + b * v[0]), 0.5 * (n[1] + a * u[1] + b * v[1]), 0.5 * (n[2] + a * u[2] + b * v[2])];
            m.vertices.push(Vertex { pos: p, normal: n, uv: [(a + 1.0) / 2.0, (b + 1.0) / 2.0] });
        }
        m.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3]);
    }
    m
}
