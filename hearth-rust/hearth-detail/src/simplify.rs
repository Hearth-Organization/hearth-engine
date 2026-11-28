//! Making a simpler version of some triangles: edges are folded away one at a time, the one that changes the shape
//! least first (Garland and Heckbert's quadric error). Edges shared with the neighbours and seams never move, so the
//! simpler version still fits exactly against whatever is next to it: that's what makes the detail crack-free.

use std::cmp::Ordering;
use std::collections::{BinaryHeap, HashMap, HashSet};

use crate::math::{cross, dot, length, sub, V3};
use crate::mesh::{edge_key, Topology, Tri};

#[derive(Clone, Copy, Default)]
struct Quadric([f64; 10]);

impl Quadric {
    fn plane(a: V3, b: V3, c: V3) -> Quadric {
        let n = cross(sub(b, a), sub(c, a));
        let l = length(n);
        if l <= 0.0 {
            return Quadric::default();
        }
        let n = [(n[0] / l) as f64, (n[1] / l) as f64, (n[2] / l) as f64];
        let d = -(n[0] * a[0] as f64 + n[1] * a[1] as f64 + n[2] * a[2] as f64);
        let [x, y, z] = n;
        Quadric([x * x, x * y, x * z, x * d, y * y, y * z, y * d, z * z, z * d, d * d])
    }

    fn add(&mut self, o: &Quadric) {
        for (a, b) in self.0.iter_mut().zip(o.0) {
            *a += b;
        }
    }

    /// The sum of squared distances from `p` to the planes this holds.
    fn error(&self, p: V3) -> f64 {
        let q = &self.0;
        let (x, y, z) = (p[0] as f64, p[1] as f64, p[2] as f64);
        let e = q[0] * x * x + 2.0 * q[1] * x * y + 2.0 * q[2] * x * z + 2.0 * q[3] * x + q[4] * y * y + 2.0 * q[5] * y * z
            + 2.0 * q[6] * y
            + q[7] * z * z
            + 2.0 * q[8] * z
            + q[9];
        e.max(0.0)
    }
}

struct Fold {
    cost: f64,
    from: u32,
    to: u32,
    stamp_from: u32,
    stamp_to: u32,
}

impl PartialEq for Fold {
    fn eq(&self, o: &Self) -> bool {
        self.cmp(o) == Ordering::Equal
    }
}
impl Eq for Fold {}
impl PartialOrd for Fold {
    fn partial_cmp(&self, o: &Self) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}
impl Ord for Fold {
    // Cheapest first out of the (max-) heap.
    fn cmp(&self, o: &Self) -> Ordering {
        o.cost.total_cmp(&self.cost).then(o.from.cmp(&self.from)).then(o.to.cmp(&self.to))
    }
}

pub struct Simplified {
    pub tris: Vec<Tri>,
    /// How far the shape moved at most, in the model's units.
    pub error: f32,
}

/// Folds edges until `target` triangles are left (or nothing more can fold without moving a locked place or
/// damaging the surface).
pub fn simplify(tris: &[Tri], topo: &Topology, target: usize) -> Simplified {
    // Local numbering for the vertices these triangles use.
    let mut local: HashMap<u32, u32> = HashMap::new();
    let mut global: Vec<u32> = Vec::new();
    let mut t: Vec<[u32; 3]> = tris
        .iter()
        .map(|tri| {
            tri.map(|v| {
                *local.entry(v).or_insert_with(|| {
                    global.push(v);
                    (global.len() - 1) as u32
                })
            })
        })
        .collect();
    let n = global.len();
    let pos: Vec<V3> = global.iter().map(|&g| topo.pos[g as usize]).collect();
    let pid: Vec<u32> = global.iter().map(|&g| topo.pid[g as usize]).collect();

    // Locked: seams, and places on an edge only one of these triangles uses (the border with the neighbours).
    let mut edge_uses: HashMap<(u32, u32), u32> = HashMap::new();
    for tri in &t {
        let p = tri.map(|v| pid[v as usize]);
        for (a, b) in [(p[0], p[1]), (p[1], p[2]), (p[2], p[0])] {
            *edge_uses.entry(edge_key(a, b)).or_default() += 1;
        }
    }
    let mut border: HashSet<u32> = HashSet::new();
    for (&(a, b), &c) in &edge_uses {
        if c != 2 {
            border.insert(a);
            border.insert(b);
        }
    }
    let locked: Vec<bool> = (0..n).map(|v| topo.seam[global[v] as usize] || border.contains(&pid[v])).collect();

    let mut quadric = vec![Quadric::default(); n];
    let mut around: Vec<Vec<usize>> = vec![Vec::new(); n];
    for (i, tri) in t.iter().enumerate() {
        let q = Quadric::plane(pos[tri[0] as usize], pos[tri[1] as usize], pos[tri[2] as usize]);
        for &v in tri {
            quadric[v as usize].add(&q);
            around[v as usize].push(i);
        }
    }
    let mut by_place: HashMap<u32, Vec<u32>> = HashMap::new();
    for (v, &p) in pid.iter().enumerate() {
        by_place.entry(p).or_default().push(v as u32);
    }

    let mut alive_tri = vec![true; t.len()];
    let mut alive_vert = vec![true; n];
    let mut stamp = vec![0u32; n];
    let mut alive = t.len();
    let mut heap = BinaryHeap::new();

    let cost = |quadric: &[Quadric], from: u32, to: u32| {
        let mut q = quadric[from as usize];
        q.add(&quadric[to as usize]);
        q.error(pos[to as usize])
    };
    let push = |heap: &mut BinaryHeap<Fold>, quadric: &[Quadric], stamp: &[u32], a: u32, b: u32| {
        if !locked[a as usize] {
            heap.push(Fold { cost: cost(quadric, a, b), from: a, to: b, stamp_from: stamp[a as usize], stamp_to: stamp[b as usize] });
        }
    };
    for tri in &t {
        for (a, b) in [(tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])] {
            push(&mut heap, &quadric, &stamp, a, b);
            push(&mut heap, &quadric, &stamp, b, a);
        }
    }

    let mut worst = 0f64;
    while alive > target {
        let Some(f) = heap.pop() else { break };
        let (u, v) = (f.from as usize, f.to as usize);
        if !alive_vert[u] || !alive_vert[v] || stamp[u] != f.stamp_from || stamp[v] != f.stamp_to {
            continue;
        }
        let tris_u: Vec<usize> = around[u].iter().copied().filter(|&i| alive_tri[i]).collect();
        let shared: Vec<usize> = tris_u.iter().copied().filter(|&i| t[i].contains(&(v as u32))).collect();
        if shared.is_empty() {
            continue; // no longer next to each other
        }

        // The surface must stay a surface: the places next to both ends are exactly the far corners of the
        // triangles on the edge (otherwise folding would pinch it).
        let places_near = |x: usize, t: &[[u32; 3]]| -> HashSet<u32> {
            let mut s = HashSet::new();
            for &lv in &by_place[&pid[x]] {
                for &i in &around[lv as usize] {
                    if alive_tri[i] {
                        s.extend(t[i].iter().map(|&w| pid[w as usize]));
                    }
                }
            }
            s.remove(&pid[x]);
            s
        };
        let near_u = places_near(u, &t);
        let near_v = places_near(v, &t);
        let both: HashSet<u32> = near_u.intersection(&near_v).copied().collect();
        let corners: HashSet<u32> = shared.iter().flat_map(|&i| t[i]).map(|w| pid[w as usize]).filter(|&p| p != pid[u] && p != pid[v]).collect();
        if both != corners {
            continue;
        }

        // No triangle may flip over or collapse to nothing.
        let flips = tris_u.iter().filter(|i| !shared.contains(*i)).any(|&i| {
            let p = |w: u32| pos[w as usize];
            let before = cross(sub(p(t[i][1]), p(t[i][0])), sub(p(t[i][2]), p(t[i][0])));
            let moved = t[i].map(|w| if w as usize == u { pos[v] } else { p(w) });
            let after = cross(sub(moved[1], moved[0]), sub(moved[2], moved[0]));
            let (lb, la) = (length(before), length(after));
            la <= lb * 1e-6 || dot(before, after) < 0.25 * lb * la
        });
        if flips {
            continue;
        }

        for &i in &tris_u {
            if shared.contains(&i) {
                alive_tri[i] = false;
                alive -= 1;
            } else {
                for w in t[i].iter_mut() {
                    if *w as usize == u {
                        *w = v as u32;
                    }
                }
                around[v].push(i);
            }
        }
        let qu = quadric[u];
        quadric[v].add(&qu);
        alive_vert[u] = false;
        stamp[v] += 1;
        worst = worst.max(f.cost);

        around[v].retain(|&i| alive_tri[i]);
        let mut next: Vec<u32> = around[v].iter().flat_map(|&i| t[i]).filter(|&w| w as usize != v).collect();
        next.sort_unstable();
        next.dedup();
        for w in next {
            push(&mut heap, &quadric, &stamp, v as u32, w);
            push(&mut heap, &quadric, &stamp, w, v as u32);
        }
    }

    let out = t.iter().zip(&alive_tri).filter(|(_, a)| **a).map(|(tri, _)| tri.map(|v| global[v as usize])).collect();
    Simplified { tris: out, error: worst.sqrt() as f32 }
}
