//! A model's triangles, and the facts every step needs about them.

use std::collections::HashMap;

use crate::math::{cross, length, V3};

#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct Vertex {
    pub pos: V3,
    pub normal: V3,
    pub uv: [f32; 2],
}

/// Triangles: three `indices` into `vertices` each.
#[derive(Clone, Debug, Default)]
pub struct Mesh {
    pub vertices: Vec<Vertex>,
    pub indices: Vec<u32>,
}

impl Mesh {
    pub fn triangle_count(&self) -> usize {
        self.indices.len() / 3
    }

    /// Smooth normals from the triangles, for models that come without them.
    pub fn compute_normals(&mut self) {
        let mut n = vec![[0f32; 3]; self.vertices.len()];
        for t in self.indices.as_chunks::<3>().0 {
            let [a, b, c] = t.map(|i| i as usize);
            let f = cross(crate::math::sub(self.vertices[b].pos, self.vertices[a].pos), crate::math::sub(self.vertices[c].pos, self.vertices[a].pos));
            for &i in &[a, b, c] {
                n[i] = crate::math::add(n[i], f);
            }
        }
        for (v, n) in self.vertices.iter_mut().zip(n) {
            let l = length(n);
            v.normal = if l > 0.0 { crate::math::scale(n, 1.0 / l) } else { [0.0, 0.0, 1.0] };
        }
    }
}

/// What the builder knows about a mesh: each vertex's place (`pid`: vertices at the same spot share one), and which
/// places are seams (two vertices there with different normals or uvs: these never move, so textures don't tear).
pub struct Topology {
    pub pos: Vec<V3>,
    pub pid: Vec<u32>,
    pub seam: Vec<bool>,
}

fn bits(v: &Vertex) -> [u32; 8] {
    let f = |x: f32| if x == 0.0 { 0 } else { x.to_bits() }; // -0 and 0 are the same place
    [f(v.pos[0]), f(v.pos[1]), f(v.pos[2]), f(v.normal[0]), f(v.normal[1]), f(v.normal[2]), f(v.uv[0]), f(v.uv[1])]
}

/// Joins vertices that are exactly the same, and drops triangles with no area. Returns the cleaned mesh and its
/// topology.
pub fn prepare(mesh: &Mesh) -> (Mesh, Topology) {
    let mut same: HashMap<[u32; 8], u32> = HashMap::new();
    let mut vertices = Vec::new();
    let remap: Vec<u32> = mesh
        .vertices
        .iter()
        .map(|v| {
            *same.entry(bits(v)).or_insert_with(|| {
                vertices.push(*v);
                (vertices.len() - 1) as u32
            })
        })
        .collect();

    let mut places: HashMap<[u32; 3], u32> = HashMap::new();
    let mut count: Vec<u32> = Vec::new();
    let pid: Vec<u32> = vertices
        .iter()
        .map(|v| {
            let b = bits(v);
            let id = *places.entry([b[0], b[1], b[2]]).or_insert_with(|| {
                count.push(0);
                (count.len() - 1) as u32
            });
            count[id as usize] += 1;
            id
        })
        .collect();

    let mut indices = Vec::with_capacity(mesh.indices.len());
    for t in mesh.indices.as_chunks::<3>().0 {
        let t = t.map(|i| remap[i as usize]);
        let p = [pid[t[0] as usize], pid[t[1] as usize], pid[t[2] as usize]];
        if p[0] != p[1] && p[1] != p[2] && p[0] != p[2] {
            indices.extend_from_slice(&t);
        }
    }
    let topo = Topology { pos: vertices.iter().map(|v| v.pos).collect(), pid, seam: count.iter().map(|&c| c > 1).collect() };
    (Mesh { vertices, indices }, topo)
}

/// The two places of an edge, smaller first, so both triangles beside it agree.
pub fn edge_key(a: u32, b: u32) -> (u32, u32) {
    if a < b {
        (a, b)
    } else {
        (b, a)
    }
}

pub type Tri = [u32; 3];
