use std::collections::HashMap;

use hearth_detail::cluster::MAX_TRIS;
use hearth_detail::mesh::{edge_key, prepare, Tri};
use hearth_detail::{build, format, load, Detail, Mesh, Vertex};

/// A closed ball made of 20 * 4^steps triangles.
fn ball(steps: u32) -> Mesh {
    let t = (1.0 + 5f32.sqrt()) / 2.0;
    let mut p: Vec<[f32; 3]> = vec![
        [-1.0, t, 0.0], [1.0, t, 0.0], [-1.0, -t, 0.0], [1.0, -t, 0.0],
        [0.0, -1.0, t], [0.0, 1.0, t], [0.0, -1.0, -t], [0.0, 1.0, -t],
        [t, 0.0, -1.0], [t, 0.0, 1.0], [-t, 0.0, -1.0], [-t, 0.0, 1.0],
    ];
    let mut f: Vec<[u32; 3]> = vec![
        [0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8],
        [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1],
    ];
    for _ in 0..steps {
        let mut mid: HashMap<(u32, u32), u32> = HashMap::new();
        let mut half = |a: u32, b: u32, p: &mut Vec<[f32; 3]>| {
            *mid.entry(edge_key(a, b)).or_insert_with(|| {
                let (x, y) = (p[a as usize], p[b as usize]);
                p.push([(x[0] + y[0]) / 2.0, (x[1] + y[1]) / 2.0, (x[2] + y[2]) / 2.0]);
                (p.len() - 1) as u32
            })
        };
        let mut next = Vec::new();
        for [a, b, c] in f {
            let (ab, bc, ca) = (half(a, b, &mut p), half(b, c, &mut p), half(c, a, &mut p));
            next.extend([[a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]]);
        }
        f = next;
    }
    let vertices = p
        .iter()
        .map(|v| {
            let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
            let n = [v[0] / l, v[1] / l, v[2] / l];
            Vertex { pos: n, normal: n, uv: [0.0, 0.0] }
        })
        .collect();
    Mesh { vertices, indices: f.into_iter().flatten().collect() }
}

/// A bumpy square sheet (open edges) with a texture seam down the middle.
fn sheet(n: u32) -> Mesh {
    let mut vertices = Vec::new();
    let mut indices = Vec::new();
    let half = n / 2;
    let mut id = HashMap::new();
    let mut vert = |x: u32, y: u32, right: bool, vertices: &mut Vec<Vertex>| -> u32 {
        *id.entry((x, y, right && x == half)).or_insert_with(|| {
            let (fx, fy) = (x as f32 / n as f32, y as f32 / n as f32);
            let z = 0.05 * (fx * 17.0).sin() * (fy * 13.0).cos();
            let u = if right && x == half { 1.0 } else { fx };
            vertices.push(Vertex { pos: [fx, fy, z], normal: [0.0, 0.0, 1.0], uv: [u, fy] });
            (vertices.len() - 1) as u32
        })
    };
    for y in 0..n {
        for x in 0..n {
            let right = x >= half;
            let a = vert(x, y, right, &mut vertices);
            let b = vert(x + 1, y, right, &mut vertices);
            let c = vert(x + 1, y + 1, right, &mut vertices);
            let d = vert(x, y + 1, right, &mut vertices);
            indices.extend([a, b, c, a, c, d]);
        }
    }
    Mesh { vertices, indices }
}

/// How many triangles use each edge (by place, so a seam doesn't count as an edge).
fn edge_uses(d: &Detail, tris: &[Tri]) -> HashMap<(u32, u32), u32> {
    let (_, topo) = prepare(&Mesh { vertices: d.vertices.clone(), indices: vec![] });
    let mut uses = HashMap::new();
    for t in tris {
        let p = t.map(|v| topo.pid[v as usize]);
        for (a, b) in [(p[0], p[1]), (p[1], p[2]), (p[2], p[0])] {
            *uses.entry(edge_key(a, b)).or_insert(0) += 1;
        }
    }
    uses
}

fn check_shape(d: &Detail, source_tris: usize, shrink: usize) {
    assert!(d.levels >= 3, "only {} levels", d.levels);
    for c in &d.clusters {
        assert!(c.index_count as usize / 3 <= MAX_TRIS && c.index_count > 0);
        assert!(c.parent_error > c.error, "a simpler version must have a bigger error");
    }
    let full = d.triangles(&d.cut(0.0)).len();
    assert_eq!(full, source_tris, "the closest view is the whole model");
    let far = d.triangles(&d.cut(f32::MAX)).len();
    assert!(far * shrink < source_tris, "the farthest view should be far smaller: {far} of {source_tris}");
}

fn thresholds(d: &Detail) -> Vec<f32> {
    let mut e: Vec<f32> = d.clusters.iter().map(|c| c.error).collect();
    e.sort_by(f32::total_cmp);
    e.dedup();
    let mut t = vec![0.0, f32::MAX];
    t.extend(e.iter().copied());
    t.extend(e.windows(2).map(|w| (w[0] + w[1]) / 2.0));
    t
}

#[test]
fn a_closed_ball_never_cracks_at_any_distance() {
    let m = ball(5); // 20480 triangles
    let d = build(&m).unwrap();
    check_shape(&d, m.triangle_count(), 10);
    let mut last = usize::MAX;
    let mut ts = thresholds(&d);
    ts.sort_by(f32::total_cmp);
    for t in ts {
        let tris = d.triangles(&d.cut(t));
        assert!(!tris.is_empty());
        for (edge, n) in edge_uses(&d, &tris) {
            assert_eq!(n, 2, "at error {t}, edge {edge:?} is used by {n} triangles: a crack or a fold");
        }
        assert!(tris.len() <= last, "farther away must never mean more triangles");
        last = tris.len();
    }
}

#[test]
fn an_open_sheet_keeps_its_outline_and_its_seam() {
    let m = sheet(64); // 8192 triangles
    let d = build(&m).unwrap();
    // Its outline and seam can never move, so it can't get as small as the ball.
    check_shape(&d, m.triangle_count(), 3);
    let full = d.triangles(&d.cut(0.0));
    let outline = |tris: &[Tri]| -> Vec<(u32, u32)> {
        let mut o: Vec<(u32, u32)> = edge_uses(&d, tris).into_iter().filter(|(_, n)| *n == 1).map(|(e, _)| e).collect();
        o.sort();
        o
    };
    let start = outline(&full);
    for t in thresholds(&d) {
        let tris = d.triangles(&d.cut(t));
        assert!(edge_uses(&d, &tris).values().all(|&n| n <= 2), "at error {t} an edge is used three times");
        assert_eq!(outline(&tris), start, "at error {t} the outline or the seam moved: a crack would show");
    }
    // The seam's two sides keep their own uvs.
    let far = d.triangles(&d.cut(f32::MAX));
    let us: Vec<f32> = far.iter().flatten().map(|&v| d.vertices[v as usize].uv[0]).collect();
    assert!(us.iter().any(|&u| u == 1.0) && us.iter().any(|&u| u > 0.0 && u < 0.5));
}

#[test]
fn errors_shrink_toward_the_full_detail_and_bounds_hold_their_pieces() {
    let d = build(&ball(4)).unwrap();
    for c in &d.clusters {
        if c.level == 0 {
            assert_eq!(c.error, 0.0);
        }
        // The ball an error is judged from must hold the pieces' own, so a piece never looks simpler than its parent.
        let gap = ((c.lod_bounds.center[0] - c.parent_bounds.center[0]).powi(2)
            + (c.lod_bounds.center[1] - c.parent_bounds.center[1]).powi(2)
            + (c.lod_bounds.center[2] - c.parent_bounds.center[2]).powi(2))
        .sqrt();
        if c.parent_error.is_finite() {
            assert!(gap + c.lod_bounds.radius <= c.parent_bounds.radius * 1.0001 + 1e-5, "parent bounds must hold the piece's");
        }
        // Every corner sits inside the piece's ball.
        let ix = &d.indices[c.first_index as usize..(c.first_index + c.index_count) as usize];
        for &v in ix {
            let p = d.vertices[v as usize].pos;
            let r = ((p[0] - c.bounds.center[0]).powi(2) + (p[1] - c.bounds.center[1]).powi(2) + (p[2] - c.bounds.center[2]).powi(2)).sqrt();
            assert!(r <= c.bounds.radius * 1.0001 + 1e-5);
        }
    }
}

#[test]
fn the_file_reads_back_the_same() {
    let d = build(&ball(3)).unwrap();
    let bytes = format::write(&d);
    let back = format::read(&bytes).unwrap();
    assert_eq!(back.vertices, d.vertices);
    assert_eq!(back.indices, d.indices);
    assert_eq!(back.clusters, d.clusters);
    assert_eq!(back.levels, d.levels);
    assert!(format::read(&bytes[..bytes.len() - 1]).is_err());
    assert!(format::read(b"nope").is_err());
}

#[test]
fn models_load() {
    let m = load::obj("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nf 1/1 2/1 3/1 4/1\nf -4 -2 -1\n").unwrap();
    assert_eq!(m.triangle_count(), 3);
    assert!(m.vertices.iter().all(|v| (v.normal[2] - 1.0).abs() < 1e-6), "normals worked out from the faces");
    assert!(load::obj("f 1 2 3\n").is_err());

    // A small .glb: one triangle with 16-bit indices.
    let json = br#"{"asset":{"version":"2.0"},"buffers":[{"byteLength":44}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}]}"#;
    let mut json = json.to_vec();
    while json.len() % 4 != 0 {
        json.push(b' ');
    }
    let mut bin = Vec::new();
    for x in [0f32, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0] {
        bin.extend_from_slice(&x.to_le_bytes());
    }
    for i in [0u16, 1, 2, 0] {
        bin.extend_from_slice(&i.to_le_bytes());
    }
    let mut glb = Vec::new();
    glb.extend_from_slice(b"glTF");
    glb.extend_from_slice(&2u32.to_le_bytes());
    glb.extend_from_slice(&((12 + 8 + json.len() + 8 + bin.len()) as u32).to_le_bytes());
    glb.extend_from_slice(&(json.len() as u32).to_le_bytes());
    glb.extend_from_slice(&0x4E4F534Au32.to_le_bytes());
    glb.extend_from_slice(&json);
    glb.extend_from_slice(&(bin.len() as u32).to_le_bytes());
    glb.extend_from_slice(&0x004E4942u32.to_le_bytes());
    glb.extend_from_slice(&bin);
    let m = load::glb(&glb).unwrap();
    assert_eq!(m.indices, [0, 1, 2]);
    assert_eq!(m.vertices[1].pos, [1.0, 0.0, 0.0]);
}

#[test]
fn bad_models_are_explained() {
    assert!(build(&Mesh::default()).unwrap_err().contains("no triangles"));
    let m = Mesh { vertices: vec![Vertex::default(); 2], indices: vec![0, 1, 5] };
    assert!(build(&m).unwrap_err().contains("vertex 5"));
}
