//! The `.hdetail` file the engine loads: everything little-endian.
//!
//! ```text
//! "HDTL", format (u32 = 1), vertex count, index count, cluster count, levels (u32 each)
//! vertices: position xyz, normal xyz, uv (8 f32 each)
//! indices: u32 each, three per triangle, into the vertices
//! clusters: first index, index count, level (u32), bounds (centre xyz, radius), error, lod bounds (4 f32),
//!           parent error, parent bounds (4 f32); a parent error of +infinity means nothing replaces it
//! ```

use crate::build::{Cluster, Detail};
use crate::math::Sphere;
use crate::mesh::Vertex;

pub const MAGIC: &[u8; 4] = b"HDTL";
pub const FORMAT: u32 = 1;

fn put_f(out: &mut Vec<u8>, x: f32) {
    out.extend_from_slice(&x.to_le_bytes());
}

fn put_u(out: &mut Vec<u8>, x: u32) {
    out.extend_from_slice(&x.to_le_bytes());
}

fn put_sphere(out: &mut Vec<u8>, s: &Sphere) {
    for x in [s.center[0], s.center[1], s.center[2], s.radius] {
        put_f(out, x);
    }
}

pub fn write(d: &Detail) -> Vec<u8> {
    let mut out = Vec::with_capacity(24 + d.vertices.len() * 32 + d.indices.len() * 4 + d.clusters.len() * 68);
    out.extend_from_slice(MAGIC);
    for x in [FORMAT, d.vertices.len() as u32, d.indices.len() as u32, d.clusters.len() as u32, d.levels] {
        put_u(&mut out, x);
    }
    for v in &d.vertices {
        for x in [v.pos[0], v.pos[1], v.pos[2], v.normal[0], v.normal[1], v.normal[2], v.uv[0], v.uv[1]] {
            put_f(&mut out, x);
        }
    }
    for &i in &d.indices {
        put_u(&mut out, i);
    }
    for c in &d.clusters {
        put_u(&mut out, c.first_index);
        put_u(&mut out, c.index_count);
        put_u(&mut out, c.level);
        put_sphere(&mut out, &c.bounds);
        put_f(&mut out, c.error);
        put_sphere(&mut out, &c.lod_bounds);
        put_f(&mut out, c.parent_error);
        put_sphere(&mut out, &c.parent_bounds);
    }
    out
}

struct Reader<'a> {
    b: &'a [u8],
    at: usize,
}

impl Reader<'_> {
    fn u(&mut self) -> Result<u32, String> {
        let s = self.b.get(self.at..self.at + 4).ok_or("the file ends too soon")?;
        self.at += 4;
        Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }
    fn f(&mut self) -> Result<f32, String> {
        Ok(f32::from_bits(self.u()?))
    }
    fn sphere(&mut self) -> Result<Sphere, String> {
        Ok(Sphere { center: [self.f()?, self.f()?, self.f()?], radius: self.f()? })
    }
}

pub fn read(b: &[u8]) -> Result<Detail, String> {
    if b.get(..4) != Some(MAGIC.as_slice()) {
        return Err("this isn't a Hearth detail file".into());
    }
    let mut r = Reader { b, at: 4 };
    let format = r.u()?;
    if format != FORMAT {
        return Err(format!("this detail file is format {format}; this reads format {FORMAT}"));
    }
    let (nv, ni, nc, levels) = (r.u()? as usize, r.u()? as usize, r.u()? as usize, r.u()?);
    let need = 24 + nv * 32 + ni * 4 + nc * 68;
    if b.len() < need {
        return Err("the file ends too soon".into());
    }
    let mut vertices = Vec::with_capacity(nv);
    for _ in 0..nv {
        vertices.push(Vertex { pos: [r.f()?, r.f()?, r.f()?], normal: [r.f()?, r.f()?, r.f()?], uv: [r.f()?, r.f()?] });
    }
    let mut indices = Vec::with_capacity(ni);
    for _ in 0..ni {
        let i = r.u()?;
        if i as usize >= nv {
            return Err(format!("a triangle uses vertex {i}, but the file has {nv}"));
        }
        indices.push(i);
    }
    let mut clusters = Vec::with_capacity(nc);
    for _ in 0..nc {
        let c = Cluster {
            first_index: r.u()?,
            index_count: r.u()?,
            level: r.u()?,
            bounds: r.sphere()?,
            error: r.f()?,
            lod_bounds: r.sphere()?,
            parent_error: r.f()?,
            parent_bounds: r.sphere()?,
        };
        if c.first_index as usize + c.index_count as usize > ni {
            return Err("a piece points past the end of the triangles".into());
        }
        clusters.push(c);
    }
    Ok(Detail { vertices, indices, clusters, levels })
}
