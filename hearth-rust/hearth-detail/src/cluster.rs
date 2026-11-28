//! Cutting triangles into compact pieces ("clusters") of up to 128 triangles, the size the graphics card handles
//! best.

use std::collections::{HashMap, HashSet};

use crate::math::{add, bbox, dist2, morton, scale, V3};
use crate::mesh::{edge_key, Topology, Tri};

pub const MAX_TRIS: usize = 128;
pub const MAX_VERTS: usize = 255;

fn centroid(t: &Tri, topo: &Topology) -> V3 {
    let p = |i: usize| topo.pos[t[i] as usize];
    scale(add(add(p(0), p(1)), p(2)), 1.0 / 3.0)
}

/// Which triangles touch which (sharing an edge), by place.
pub fn neighbours(tris: &[Tri], topo: &Topology) -> Vec<Vec<usize>> {
    let mut by_edge: HashMap<(u32, u32), Vec<usize>> = HashMap::new();
    for (i, t) in tris.iter().enumerate() {
        let p = t.map(|v| topo.pid[v as usize]);
        for (a, b) in [(p[0], p[1]), (p[1], p[2]), (p[2], p[0])] {
            by_edge.entry(edge_key(a, b)).or_default().push(i);
        }
    }
    let mut out = vec![Vec::new(); tris.len()];
    for list in by_edge.values() {
        for &a in list {
            for &b in list {
                if a != b && !out[a].contains(&b) {
                    out[a].push(b);
                }
            }
        }
    }
    out
}

/// Splits triangles into clusters: each grows from a seed, always taking the touching triangle nearest its middle,
/// so clusters come out round and compact (which makes them cheap to skip when they're off screen).
pub fn clusterize(tris: &[Tri], topo: &Topology) -> Vec<Vec<Tri>> {
    if tris.is_empty() {
        return Vec::new();
    }
    let cent: Vec<V3> = tris.iter().map(|t| centroid(t, topo)).collect();
    let near = neighbours(tris, topo);
    let (min, size) = bbox(cent.iter().copied());
    let mut order: Vec<usize> = (0..tris.len()).collect();
    order.sort_by_key(|&i| morton(cent[i], min, size));

    let mut taken = vec![false; tris.len()];
    let mut out = Vec::new();
    for &seed in &order {
        if taken[seed] {
            continue;
        }
        let mut members: Vec<Tri> = Vec::new();
        let mut verts: HashSet<u32> = HashSet::new();
        let mut sum = [0f32; 3];
        let mut frontier: Vec<usize> = Vec::new();
        let mut next = Some(seed);
        while let Some(t) = next {
            taken[t] = true;
            members.push(tris[t]);
            verts.extend(tris[t]);
            sum = add(sum, cent[t]);
            frontier.extend(near[t].iter().copied().filter(|&n| !taken[n]));
            if members.len() >= MAX_TRIS {
                break;
            }
            let middle = scale(sum, 1.0 / members.len() as f32);
            frontier.retain(|&n| !taken[n]);
            next = None;
            let mut best = f32::MAX;
            for (k, &n) in frontier.iter().enumerate() {
                let extra = tris[n].iter().filter(|v| !verts.contains(v)).count();
                if verts.len() + extra > MAX_VERTS {
                    continue;
                }
                let d = dist2(cent[n], middle);
                if d < best {
                    best = d;
                    next = Some(k);
                }
            }
            next = next.map(|k| frontier.swap_remove(k));
        }
        out.push(members);
    }
    out
}
