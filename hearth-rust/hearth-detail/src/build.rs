//! Building the detail levels the way Nanite does: pieces are put in groups of neighbours, each group is simplified
//! to half its triangles with its outside edges kept still, and the result is cut into new pieces. That repeats until
//! one piece (or nothing simpler) is left.
//!
//! Every piece knows its own error and its parent group's error. The game draws a piece when its own error looks
//! small enough from where the camera is and its parent's doesn't: those pieces always fit together with no cracks,
//! whatever mix of levels is on screen.

use std::collections::HashMap;

use crate::cluster::{clusterize, MAX_TRIS};
use crate::math::{add, bbox, morton, scale, Sphere, V3};
use crate::mesh::{edge_key, prepare, Mesh, Topology, Tri, Vertex};
use crate::simplify::simplify;

/// How many neighbouring pieces are simplified together.
pub const GROUP_SIZE: usize = 8;

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Cluster {
    /// Its triangles: `index_count` indices from `first_index` in `Detail::indices`.
    pub first_index: u32,
    pub index_count: u32,
    /// 0 is the full detail.
    pub level: u32,
    /// Holds its triangles (for skipping pieces off screen).
    pub bounds: Sphere,
    /// How far its shape is from the full detail, and the ball that error is judged from.
    pub error: f32,
    pub lod_bounds: Sphere,
    /// The same for the simpler version that replaces it (infinite: nothing does).
    pub parent_error: f32,
    pub parent_bounds: Sphere,
}

#[derive(Clone, Debug)]
pub struct Detail {
    pub vertices: Vec<Vertex>,
    pub indices: Vec<u32>,
    pub clusters: Vec<Cluster>,
    pub levels: u32,
}

impl Detail {
    /// The pieces to draw when an error of `max_error` (in the model's units) is fine everywhere. The game does this
    /// per piece on the graphics card with the error as seen from the camera; this is the same rule, for tools and
    /// tests.
    pub fn cut(&self, max_error: f32) -> Vec<usize> {
        self.clusters.iter().enumerate().filter(|(_, c)| c.error <= max_error && c.parent_error > max_error).map(|(i, _)| i).collect()
    }

    /// The pieces to draw from `camera` (in the model's own space): each one whose error looks at most `max_pixels`
    /// on screen while its simpler version's doesn't. `pixels_per_unit`: how many pixels one unit, one unit away,
    /// covers (screen height / (2 tan(fov / 2))). The model's scale cancels out, so it needs no scaling.
    pub fn cut_for_camera(&self, camera: [f32; 3], pixels_per_unit: f32, max_pixels: f32) -> Vec<usize> {
        let looks = |error: f32, s: &crate::math::Sphere| {
            if error.is_infinite() {
                return f32::INFINITY;
            }
            let d = crate::math::dist2(s.center, camera).sqrt() - s.radius;
            error / d.max(1e-3) * pixels_per_unit
        };
        self.clusters
            .iter()
            .enumerate()
            .filter(|(_, c)| looks(c.error, &c.lod_bounds) <= max_pixels && looks(c.parent_error, &c.parent_bounds) > max_pixels)
            .map(|(i, _)| i)
            .collect()
    }

    pub fn triangles(&self, clusters: &[usize]) -> Vec<Tri> {
        let mut out = Vec::new();
        for &c in clusters {
            let c = &self.clusters[c];
            let ix = &self.indices[c.first_index as usize..(c.first_index + c.index_count) as usize];
            out.extend(ix.as_chunks::<3>().0.iter().copied());
        }
        out
    }
}

struct Piece {
    tris: Vec<Tri>,
    level: u32,
    error: f32,
    lod_bounds: Sphere,
    parent_error: f32,
    parent_bounds: Sphere,
}

fn bounds_of(tris: &[Tri], topo: &Topology) -> Sphere {
    Sphere::of_points(tris.iter().flatten().map(|&v| topo.pos[v as usize]))
}

fn middle(tris: &[Tri], topo: &Topology) -> V3 {
    let sum = tris.iter().flatten().fold([0f32; 3], |s, &v| add(s, topo.pos[v as usize]));
    scale(sum, 1.0 / (tris.len() * 3).max(1) as f32)
}

/// Puts pieces in groups of up to GROUP_SIZE that share the most edges, so each group is one round patch.
fn group(ids: &[usize], pieces: &[Piece], topo: &Topology) -> Vec<Vec<usize>> {
    let mut by_edge: HashMap<(u32, u32), Vec<usize>> = HashMap::new();
    for (k, &id) in ids.iter().enumerate() {
        for t in &pieces[id].tris {
            let p = t.map(|v| topo.pid[v as usize]);
            for (a, b) in [(p[0], p[1]), (p[1], p[2]), (p[2], p[0])] {
                let list = by_edge.entry(edge_key(a, b)).or_default();
                if !list.contains(&k) {
                    list.push(k);
                }
            }
        }
    }
    let mut shared: Vec<HashMap<usize, u32>> = vec![HashMap::new(); ids.len()];
    for list in by_edge.values() {
        for &a in list {
            for &b in list {
                if a != b {
                    *shared[a].entry(b).or_default() += 1;
                }
            }
        }
    }
    let mids: Vec<V3> = ids.iter().map(|&id| middle(&pieces[id].tris, topo)).collect();
    let (min, size) = bbox(mids.iter().copied());
    let mut order: Vec<usize> = (0..ids.len()).collect();
    order.sort_by_key(|&k| morton(mids[k], min, size));

    let mut in_group: Vec<Option<usize>> = vec![None; ids.len()];
    let mut groups: Vec<Vec<usize>> = Vec::new();
    for &seed in &order {
        if in_group[seed].is_some() {
            continue;
        }
        let g = groups.len();
        let mut members = vec![seed];
        in_group[seed] = Some(g);
        let mut pull: HashMap<usize, u32> = HashMap::new();
        let mut last = seed;
        while members.len() < GROUP_SIZE {
            for (&n, &w) in &shared[last] {
                if in_group[n].is_none() {
                    *pull.entry(n).or_default() += w;
                }
            }
            pull.retain(|n, _| in_group[*n].is_none());
            let Some((&best, _)) = pull.iter().max_by(|a, b| a.1.cmp(b.1).then(b.0.cmp(a.0))) else { break };
            members.push(best);
            in_group[best] = Some(g);
            pull.remove(&best);
            last = best;
        }
        groups.push(members);
    }

    // A piece left on its own can hardly be simplified (its whole edge is kept still): it joins the neighbouring
    // group it shares the most with.
    let lone_groups: Vec<usize> = groups.iter().enumerate().filter(|(_, g)| g.len() == 1).map(|(k, _)| k).collect();
    for k in lone_groups {
        if groups[k].len() != 1 {
            continue;
        }
        let lone = groups[k][0];
        let best = shared[lone]
            .iter()
            .filter_map(|(&n, &w)| in_group[n].map(|g| (g, w)))
            .filter(|&(g, _)| g != k && groups[g].len() < GROUP_SIZE * 2)
            .max_by(|a, b| a.1.cmp(&b.1).then(b.0.cmp(&a.0)));
        if let Some((g, _)) = best {
            groups[k].clear();
            groups[g].push(lone);
            in_group[lone] = Some(g);
        }
    }
    groups.retain(|g| !g.is_empty());
    groups.into_iter().map(|g| g.into_iter().map(|k| ids[k]).collect()).collect()
}

/// Builds every detail level for a model.
pub fn build(mesh: &Mesh) -> Result<Detail, String> {
    if !mesh.indices.len().is_multiple_of(3) {
        return Err("the model's triangle list isn't whole (its length isn't a multiple of 3)".into());
    }
    if let Some(&bad) = mesh.indices.iter().find(|&&i| i as usize >= mesh.vertices.len()) {
        return Err(format!("a triangle uses vertex {bad}, but the model has {}", mesh.vertices.len()));
    }
    let (mesh, topo) = prepare(mesh);
    if mesh.indices.is_empty() {
        return Err("the model has no triangles".into());
    }
    let tris: Vec<Tri> = mesh.indices.as_chunks::<3>().0.to_vec();

    let mut pieces: Vec<Piece> = Vec::new();
    let mut current: Vec<usize> = Vec::new();
    for c in clusterize(&tris, &topo) {
        let b = bounds_of(&c, &topo);
        current.push(pieces.len());
        pieces.push(Piece { tris: c, level: 0, error: 0.0, lod_bounds: b, parent_error: f32::INFINITY, parent_bounds: b });
    }

    let mut level = 0;
    while current.len() > 1 && level < 64 {
        let mut next = Vec::new();
        for members in group(&current, &pieces, &topo) {
            let tris: Vec<Tri> = members.iter().flat_map(|&m| pieces[m].tris.iter().copied()).collect();
            let s = simplify(&tris, &topo, tris.len() / 2);
            if s.tris.is_empty() || s.tris.len() as f32 > tris.len() as f32 * 0.85 {
                continue; // can't get simpler: these pieces stay as they are, at every distance
            }
            let child_error = members.iter().map(|&m| pieces[m].error).fold(0f32, f32::max);
            // Always above every child's error, even when simplifying moved nothing (a flat patch).
            let summed = child_error + s.error;
            let error = if summed > child_error { summed } else { f32::from_bits(child_error.to_bits() + 1) };
            let lod_bounds = members.iter().map(|&m| pieces[m].lod_bounds).reduce(Sphere::merge).unwrap_or_default();
            for &m in &members {
                pieces[m].parent_error = error;
                pieces[m].parent_bounds = lod_bounds;
            }
            for c in clusterize(&s.tris, &topo) {
                next.push(pieces.len());
                pieces.push(Piece { tris: c, level: level + 1, error, lod_bounds, parent_error: f32::INFINITY, parent_bounds: lod_bounds });
            }
        }
        if next.is_empty() {
            break;
        }
        current = next;
        level += 1;
    }

    let mut indices = Vec::new();
    let mut clusters = Vec::with_capacity(pieces.len());
    for p in &pieces {
        debug_assert!(p.tris.len() <= MAX_TRIS);
        let first = indices.len() as u32;
        indices.extend(p.tris.iter().flatten());
        clusters.push(Cluster {
            first_index: first,
            index_count: (p.tris.len() * 3) as u32,
            level: p.level,
            bounds: bounds_of(&p.tris, &topo),
            error: p.error,
            lod_bounds: p.lod_bounds,
            parent_error: p.parent_error,
            parent_bounds: p.parent_bounds,
        });
    }
    Ok(Detail { vertices: mesh.vertices, indices, clusters, levels: level + 1 })
}
