//! Reading models: OBJ, and GLB (binary glTF, what Hearth Forge makes).

use std::collections::HashMap;

use serde_json::Value as Json;

use crate::mesh::{Mesh, Vertex};

pub fn load(path: &std::path::Path) -> Result<Mesh, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("can't open {}: {e}", path.display()))?;
    match path.extension().and_then(|e| e.to_str()).map(str::to_ascii_lowercase).as_deref() {
        Some("obj") => obj(&String::from_utf8_lossy(&bytes)),
        Some("glb") => glb(&bytes),
        _ => Err("this reads .obj and .glb models".into()),
    }
}

/// OBJ: `v`, `vt`, `vn` and `f` (any number of corners; negative numbers count back from the end).
pub fn obj(text: &str) -> Result<Mesh, String> {
    let (mut p, mut t, mut n) = (Vec::new(), Vec::new(), Vec::new());
    let mut mesh = Mesh::default();
    let mut seen: HashMap<(usize, Option<usize>, Option<usize>), u32> = HashMap::new();
    let mut has_normals = true;
    for (line_no, line) in text.lines().enumerate() {
        let mut w = line.split_whitespace();
        let bad = |what: &str| format!("line {}: {what}", line_no + 1);
        let nums = |w: &mut std::str::SplitWhitespace, k: usize| -> Result<Vec<f32>, String> {
            (0..k).map(|_| w.next().and_then(|x| x.parse().ok()).ok_or_else(|| bad("a number is missing"))).collect()
        };
        match w.next() {
            Some("v") => p.push(nums(&mut w, 3)?),
            Some("vt") => t.push(nums(&mut w, 2)?),
            Some("vn") => n.push(nums(&mut w, 3)?),
            Some("f") => {
                let mut corners = Vec::new();
                for c in w {
                    let mut parts = c.split('/');
                    let index = |s: Option<&str>, len: usize| -> Result<Option<usize>, String> {
                        match s.filter(|s| !s.is_empty()) {
                            None => Ok(None),
                            Some(s) => {
                                let i: i64 = s.parse().map_err(|_| bad("a corner isn't a number"))?;
                                let i = if i < 0 { len as i64 + i } else { i - 1 };
                                if i < 0 || i as usize >= len {
                                    return Err(bad("a corner points to something that isn't there"));
                                }
                                Ok(Some(i as usize))
                            }
                        }
                    };
                    let vi = index(parts.next(), p.len())?.ok_or_else(|| bad("a corner has no position"))?;
                    let ti = index(parts.next(), t.len())?;
                    let ni = index(parts.next(), n.len())?;
                    has_normals &= ni.is_some();
                    let id = *seen.entry((vi, ti, ni)).or_insert_with(|| {
                        let pos = &p[vi];
                        mesh.vertices.push(Vertex {
                            pos: [pos[0], pos[1], pos[2]],
                            normal: ni.map(|i: usize| [n[i][0], n[i][1], n[i][2]]).unwrap_or_default(),
                            uv: ti.map(|i: usize| [t[i][0], t[i][1]]).unwrap_or_default(),
                        });
                        (mesh.vertices.len() - 1) as u32
                    });
                    corners.push(id);
                }
                if corners.len() >= 3 {
                    for w in corners[1..].windows(2) {
                        mesh.indices.extend_from_slice(&[corners[0], w[0], w[1]]);
                    }
                }
            }
            _ => {}
        }
    }
    if !has_normals {
        mesh.compute_normals();
    }
    Ok(mesh)
}

fn accessor(json: &Json, bin: &[u8], index: usize) -> Result<(Vec<f32>, usize), String> {
    let a = &json["accessors"][index];
    let count = a["count"].as_u64().ok_or("an accessor has no count")? as usize;
    let comps = match a["type"].as_str() {
        Some("SCALAR") => 1,
        Some("VEC2") => 2,
        Some("VEC3") => 3,
        Some("VEC4") => 4,
        other => return Err(format!("accessor type {other:?} isn't used for shapes")),
    };
    let ctype = a["componentType"].as_u64().unwrap_or(0);
    let size = match ctype {
        5126 | 5125 => 4,
        5123 | 5122 => 2,
        5121 | 5120 => 1,
        _ => return Err(format!("component type {ctype} isn't supported")),
    };
    let normalized = a["normalized"].as_bool().unwrap_or(false);
    let Some(view) = a["bufferView"].as_u64() else {
        return Ok((vec![0.0; count * comps], comps)); // no data: all zeros (the glTF rule)
    };
    let v = &json["bufferViews"][view as usize];
    if v["buffer"].as_u64().unwrap_or(0) != 0 {
        return Err("the model keeps data outside the .glb".into());
    }
    let start = v["byteOffset"].as_u64().unwrap_or(0) as usize + a["byteOffset"].as_u64().unwrap_or(0) as usize;
    let stride = v["byteStride"].as_u64().map(|s| s as usize).unwrap_or(size * comps);
    let mut out = Vec::with_capacity(count * comps);
    for i in 0..count {
        for c in 0..comps {
            let at = start + i * stride + c * size;
            let b = bin.get(at..at + size).ok_or("the model's data ends too soon")?;
            let x = match ctype {
                5126 => f32::from_le_bytes([b[0], b[1], b[2], b[3]]),
                5125 => u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as f32,
                5123 => u16::from_le_bytes([b[0], b[1]]) as f32 / if normalized { 65535.0 } else { 1.0 },
                5122 => i16::from_le_bytes([b[0], b[1]]) as f32 / if normalized { 32767.0 } else { 1.0 },
                5121 => b[0] as f32 / if normalized { 255.0 } else { 1.0 },
                _ => (b[0] as i8) as f32 / if normalized { 127.0 } else { 1.0 },
            };
            out.push(if normalized && matches!(ctype, 5122 | 5120) { x.max(-1.0) } else { x });
        }
    }
    Ok((out, comps))
}

/// GLB: every triangle primitive of every mesh, joined (node placement isn't applied: Forge models are one mesh).
pub fn glb(b: &[u8]) -> Result<Mesh, String> {
    let u = |at: usize| b.get(at..at + 4).map(|s| u32::from_le_bytes([s[0], s[1], s[2], s[3]]) as usize).ok_or("this .glb ends too soon");
    if b.get(..4) != Some(b"glTF".as_slice()) {
        return Err("this isn't a .glb file".into());
    }
    let json_len = u(12)?;
    if u(16)? != 0x4E4F534A {
        return Err("this .glb has no JSON part".into());
    }
    let json: Json = serde_json::from_slice(b.get(20..20 + json_len).ok_or("this .glb ends too soon")?).map_err(|e| format!("this .glb's JSON is broken: {e}"))?;
    let bin_at = 20 + json_len;
    let bin: &[u8] = if b.len() >= bin_at + 8 && u(bin_at + 4)? == 0x004E4942 {
        let len = u(bin_at)?;
        b.get(bin_at + 8..bin_at + 8 + len).ok_or("this .glb ends too soon")?
    } else {
        &[]
    };

    let mut mesh = Mesh::default();
    let mut has_normals = true;
    for m in json["meshes"].as_array().into_iter().flatten() {
        for prim in m["primitives"].as_array().into_iter().flatten() {
            if prim["mode"].as_u64().unwrap_or(4) != 4 {
                continue; // lines and points aren't shapes
            }
            let at = &prim["attributes"];
            let Some(pi) = at["POSITION"].as_u64() else { continue };
            let (pos, _) = accessor(&json, bin, pi as usize)?;
            let count = pos.len() / 3;
            let normals = at["NORMAL"].as_u64().map(|i| accessor(&json, bin, i as usize).map(|r| r.0)).transpose()?;
            has_normals &= normals.is_some();
            let uvs = at["TEXCOORD_0"].as_u64().map(|i| accessor(&json, bin, i as usize).map(|r| r.0)).transpose()?;
            let base = mesh.vertices.len() as u32;
            for i in 0..count {
                mesh.vertices.push(Vertex {
                    pos: [pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]],
                    normal: normals.as_ref().and_then(|n| n.get(i * 3..i * 3 + 3)).map(|n| [n[0], n[1], n[2]]).unwrap_or_default(),
                    uv: uvs.as_ref().and_then(|t| t.get(i * 2..i * 2 + 2)).map(|t| [t[0], t[1]]).unwrap_or_default(),
                });
            }
            match prim["indices"].as_u64() {
                Some(ii) => {
                    let (ix, _) = accessor(&json, bin, ii as usize)?;
                    for i in ix {
                        let i = i as u32;
                        if i as usize >= count {
                            return Err("a triangle points past the model's vertices".into());
                        }
                        mesh.indices.push(base + i);
                    }
                }
                None => mesh.indices.extend((0..count as u32).map(|i| base + i)),
            }
        }
    }
    if mesh.indices.is_empty() {
        return Err("this .glb has no triangles".into());
    }
    if !has_normals {
        mesh.compute_normals();
    }
    Ok(mesh)
}
