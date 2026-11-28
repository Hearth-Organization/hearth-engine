//! Import checks: what's wrong with a model, in plain words, with what to do about it. Problems that would break
//! the game are "stop"; ones that look wrong are "warn"; the rest are facts ("note").

use std::collections::HashMap;

use hearth_detail::math::{cross, dot, length, sub};
use hearth_detail::mesh::{edge_key, prepare};
use hearth_detail::Mesh;
use serde_json::json;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Level {
    Note,
    Warn,
    Stop,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Finding {
    pub level: Level,
    pub what: String,
    pub fix: String,
}

#[derive(Clone, Debug)]
pub struct Report {
    pub triangles: usize,
    pub vertices: usize,
    /// Metres, assuming the model is in metres.
    pub size: [f32; 3],
    pub findings: Vec<Finding>,
}

impl Report {
    pub fn worst(&self) -> Level {
        self.findings.iter().map(|f| f.level).max().unwrap_or(Level::Note)
    }

    pub fn to_json(&self) -> serde_json::Value {
        json!({
            "triangles": self.triangles,
            "vertices": self.vertices,
            "size": self.size,
            "ok": self.worst() != Level::Stop,
            "findings": self.findings.iter().map(|f| json!({
                "level": match f.level { Level::Note => "note", Level::Warn => "warn", Level::Stop => "stop" },
                "what": f.what, "fix": f.fix })).collect::<Vec<_>>(),
        })
    }

    pub fn to_text(&self) -> String {
        let mut s = format!("{} triangles, {:.2} x {:.2} x {:.2} m\n", self.triangles, self.size[0], self.size[1], self.size[2]);
        for f in &self.findings {
            let tag = match f.level {
                Level::Note => "  ",
                Level::Warn => "! ",
                Level::Stop => "X ",
            };
            s += &format!("{tag}{} {}\n", f.what, f.fix);
        }
        s
    }
}

/// Limits a game can set (defaults suit a PC game).
#[derive(Clone, Debug)]
pub struct Limits {
    pub max_triangles: usize,
    /// Below this size (metres) a model is probably in centimetres or millimetres; above, in the wrong units too.
    pub smallest: f32,
    pub largest: f32,
    /// The engine's up axis: 2 = Z (Hearth Engine), 1 = Y.
    pub up: usize,
}

impl Default for Limits {
    fn default() -> Limits {
        Limits { max_triangles: 2_000_000, smallest: 0.02, largest: 2000.0, up: 2 }
    }
}

pub fn check(mesh: &Mesh, limits: &Limits) -> Report {
    let mut f = Vec::new();
    let mut add = |level, what: String, fix: &str| f.push(Finding { level, what, fix: fix.to_string() });
    let tris_in = mesh.triangle_count();
    if tris_in == 0 {
        add(Level::Stop, "The model has no triangles.".into(), "Export it again with the mesh included.");
        return Report { triangles: 0, vertices: mesh.vertices.len(), size: [0.0; 3], findings: f };
    }
    if mesh.vertices.iter().any(|v| v.pos.iter().any(|x| !x.is_finite())) {
        add(Level::Stop, "Some corners are at impossible places (not a number or infinite).".into(), "The file is damaged; export it again.");
    }
    let (clean, topo) = prepare(mesh);
    let tris = clean.triangle_count();
    let degenerate = tris_in - tris;
    if degenerate > 0 {
        add(Level::Note, format!("{degenerate} triangles have no area and were dropped."), "Nothing to do.");
    }

    let (mut lo, mut hi) = ([f32::MAX; 3], [f32::MIN; 3]);
    for v in &clean.vertices {
        lo = [lo[0].min(v.pos[0]), lo[1].min(v.pos[1]), lo[2].min(v.pos[2])];
        hi = [hi[0].max(v.pos[0]), hi[1].max(v.pos[1]), hi[2].max(v.pos[2])];
    }
    let size = [hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]];
    let biggest = size.iter().copied().fold(0.0, f32::max);
    if biggest < limits.smallest {
        add(Level::Warn, format!("It's only {:.4} m across: probably made in centimetres or millimetres.", biggest), "Scale it up (x100 for centimetres, x1000 for millimetres).");
    } else if biggest > limits.largest {
        add(Level::Warn, format!("It's {biggest:.0} m across: probably in the wrong units."), "Scale it down (x0.01 if it was made in centimetres).");
    }
    // Lying on its side: the tallest side should be the up axis for things that stand (most models).
    let up = limits.up.min(2);
    let other_up = if up == 2 { 1 } else { 2 };
    if size[other_up] > size[up] * 1.8 && size[other_up] > size[0] * 1.2 {
        add(Level::Warn, format!("It's tallest along {} but up is {}: it may lie on its side.", ["X", "Y", "Z"][other_up], ["X", "Y", "Z"][up]), "Turn it 90 degrees, or export with the right up axis.");
    }
    let middle = [(lo[0] + hi[0]) / 2.0, (lo[1] + hi[1]) / 2.0, lo[up]];
    let off = length([middle[0], middle[1], 0.0]);
    if off > biggest * 2.0 && off > 1.0 {
        add(Level::Warn, format!("Its middle is {off:.1} m from its origin: it will appear far from where it's placed."), "Move it to the origin before exporting.");
    }
    if tris > limits.max_triangles {
        add(Level::Warn, format!("{tris} triangles is a lot for one model."), "Use Detail Mesh (Nanite-style detail) for it, or simplify it.");
    } else {
        add(Level::Note, format!("{tris} triangles."), "");
    }

    // Edges: holes and places where more than two triangles meet.
    let mut uses: HashMap<(u32, u32), u32> = HashMap::new();
    for t in clean.indices.as_chunks::<3>().0 {
        let p = t.map(|v| topo.pid[v as usize]);
        for (a, b) in [(p[0], p[1]), (p[1], p[2]), (p[2], p[0])] {
            *uses.entry(edge_key(a, b)).or_default() += 1;
        }
    }
    let open = uses.values().filter(|&&n| n == 1).count();
    let tangled = uses.values().filter(|&&n| n > 2).count();
    if open > 0 {
        add(Level::Note, format!("It has {open} open edges (it isn't closed)."), "Fine for most things; physics needs closed shapes for solid collisions.");
    }
    if tangled > 0 {
        add(Level::Warn, format!("{tangled} edges are shared by more than two triangles."), "Clean up the mesh (merge by distance, remove inside faces).");
    }

    // Normals that point the wrong way compared to the triangle they belong to.
    let mut against = 0usize;
    let mut zero = 0usize;
    for t in clean.indices.as_chunks::<3>().0 {
        let [a, b, c] = t.map(|i| clean.vertices[i as usize].pos);
        let face = cross(sub(b, a), sub(c, a));
        if length(face) < 1e-12 {
            continue;
        }
        let n: [f32; 3] = t.iter().fold([0.0; 3], |s, &i| {
            let n = clean.vertices[i as usize].normal;
            [s[0] + n[0], s[1] + n[1], s[2] + n[2]]
        });
        if length(n) < 1e-6 {
            zero += 1;
        } else if dot(n, face) < 0.0 {
            against += 1;
        }
    }
    if zero * 10 > tris {
        add(Level::Warn, "Many corners have no normal.".into(), "Hearth Engine can work them out; export with normals for the intended look.");
    }
    if against * 4 > tris {
        add(Level::Warn, format!("{}% of the triangles face the other way from their normals: it may look inside out.", against * 100 / tris), "Recalculate normals (outside) before exporting.");
    }
    if clean.vertices.iter().all(|v| v.uv == [0.0, 0.0]) {
        add(Level::Warn, "It has no texture coordinates (uvs).".into(), "Textures won't show; unwrap it, or use plain-colour materials.");
    }
    Report { triangles: tris, vertices: clean.vertices.len(), size, findings: f }
}
