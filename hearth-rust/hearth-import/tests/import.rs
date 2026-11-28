use hearth_detail::{Mesh, Vertex};
use hearth_import::{check, Level, Limits};

/// A closed box w x d x h (Z up) with outward normals and uvs.
fn cube(w: f32, d: f32, h: f32, shift: [f32; 3]) -> Mesh {
    let mut m = Mesh::default();
    let faces: [([f32; 3], [[f32; 3]; 4]); 6] = [
        ([0.0, 0.0, 1.0], [[0.0, 0.0, 1.0], [1.0, 0.0, 1.0], [1.0, 1.0, 1.0], [0.0, 1.0, 1.0]]),
        ([0.0, 0.0, -1.0], [[0.0, 0.0, 0.0], [0.0, 1.0, 0.0], [1.0, 1.0, 0.0], [1.0, 0.0, 0.0]]),
        ([1.0, 0.0, 0.0], [[1.0, 0.0, 0.0], [1.0, 1.0, 0.0], [1.0, 1.0, 1.0], [1.0, 0.0, 1.0]]),
        ([-1.0, 0.0, 0.0], [[0.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, 1.0, 1.0], [0.0, 1.0, 0.0]]),
        ([0.0, 1.0, 0.0], [[0.0, 1.0, 0.0], [0.0, 1.0, 1.0], [1.0, 1.0, 1.0], [1.0, 1.0, 0.0]]),
        ([0.0, -1.0, 0.0], [[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [1.0, 0.0, 1.0], [0.0, 0.0, 1.0]]),
    ];
    for (n, corners) in faces {
        let base = m.vertices.len() as u32;
        for (k, c) in corners.iter().enumerate() {
            let uv = [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]][k];
            m.vertices.push(Vertex { pos: [c[0] * w + shift[0], c[1] * d + shift[1], c[2] * h + shift[2]], normal: n, uv });
        }
        m.indices.extend([base, base + 1, base + 2, base, base + 2, base + 3]);
    }
    m
}

fn whats(m: &Mesh) -> Vec<String> {
    check(m, &Limits::default()).findings.iter().filter(|f| f.level != Level::Note).map(|f| f.what.clone()).collect()
}

#[test]
fn a_good_model_has_nothing_to_warn_about() {
    let m = cube(1.0, 1.0, 2.0, [-0.5, -0.5, 0.0]);
    let r = check(&m, &Limits::default());
    assert_eq!(r.triangles, 12);
    assert_eq!(r.worst(), Level::Note, "{}", r.to_text());
    assert_eq!(r.size, [1.0, 1.0, 2.0]);
}

#[test]
fn wrong_units_sideways_and_far_away_are_spotted() {
    assert!(whats(&cube(0.01, 0.01, 0.01, [0.0; 3]))[0].contains("centimetres"));
    assert!(whats(&cube(5000.0, 5000.0, 5000.0, [0.0; 3]))[0].contains("wrong units"));
    assert!(whats(&cube(0.5, 3.0, 0.5, [-0.25, -1.5, 0.0])).iter().any(|w| w.contains("on its side")));
    assert!(whats(&cube(1.0, 1.0, 1.0, [50.0, 0.0, 0.0])).iter().any(|w| w.contains("from its origin")));
}

#[test]
fn inside_out_no_uvs_and_holes_are_explained() {
    let mut m = cube(1.0, 1.0, 1.0, [0.0; 3]);
    for v in &mut m.vertices {
        v.normal = [-v.normal[0], -v.normal[1], -v.normal[2]];
    }
    assert!(whats(&m).iter().any(|w| w.contains("inside out")));
    let mut m = cube(1.0, 1.0, 1.0, [0.0; 3]);
    for v in &mut m.vertices {
        v.uv = [0.0, 0.0];
    }
    assert!(whats(&m).iter().any(|w| w.contains("texture coordinates")));
    let mut m = cube(1.0, 1.0, 1.0, [0.0; 3]);
    m.indices.truncate(30); // take the last side off
    let r = check(&m, &Limits::default());
    assert!(r.findings.iter().any(|f| f.what.contains("open edges")));
    assert!(r.to_json()["ok"].as_bool().unwrap());
}

#[test]
fn an_empty_model_cant_be_used() {
    let r = check(&Mesh::default(), &Limits::default());
    assert_eq!(r.worst(), Level::Stop);
    assert!(!r.to_json()["ok"].as_bool().unwrap());
}
