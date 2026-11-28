//! The C interface Hearth Engine's HearthRust gem calls (header: `include/hearth_ffi.h`).
//!
//! Rules for every function: no panic ever crosses into C++ (each is caught and turned into an error), strings in
//! are UTF-8 and null-terminated, and strings out belong to the object that gave them and stay valid until the next
//! call on that object.

use std::ffi::{c_char, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use hearth_detail::{build, format, load, Detail};
use hearth_script::{Engine, Value};
use serde_json::Value as Json;

fn text<'a>(p: *const c_char) -> &'a str {
    if p.is_null() {
        return "";
    }
    unsafe { CStr::from_ptr(p) }.to_str().unwrap_or("")
}

fn keep(slot: &mut CString, s: &str) -> *const c_char {
    *slot = CString::new(s.replace('\0', " ")).unwrap_or_default();
    slot.as_ptr()
}

// ---------------------------------------------------------------- values between Hearth Script and JSON

pub fn to_json(v: &Value) -> Json {
    match v {
        Value::Nil => Json::Null,
        Value::Bool(b) => Json::Bool(*b),
        Value::Num(n) => serde_json::Number::from_f64(*n).map(Json::Number).unwrap_or(Json::Null),
        Value::Text(t) => Json::String(t.to_string()),
        Value::List(l) => Json::Array(l.borrow().iter().map(to_json).collect()),
        Value::Record(r) => Json::Object(r.borrow().iter().map(|(k, v)| (k.clone(), to_json(v))).collect()),
        Value::Func(..) | Value::Native(..) => Json::String(v.to_string()),
    }
}

pub fn from_json(j: &Json) -> Value {
    match j {
        Json::Null => Value::Nil,
        Json::Bool(b) => Value::Bool(*b),
        Json::Number(n) => Value::Num(n.as_f64().unwrap_or(0.0)),
        Json::String(s) => Value::text(s),
        Json::Array(a) => Value::list(a.iter().map(from_json).collect()),
        Json::Object(o) => Value::Record(std::rc::Rc::new(std::cell::RefCell::new(o.iter().map(|(k, v)| (k.clone(), from_json(v))).collect()))),
    }
}

fn args_from_json(s: &str) -> Vec<Value> {
    match serde_json::from_str::<Json>(s) {
        Ok(Json::Array(a)) => a.iter().map(from_json).collect(),
        Ok(Json::Null) | Err(_) => Vec::new(),
        Ok(other) => vec![from_json(&other)],
    }
}

// ---------------------------------------------------------------- Hearth Script

/// A game function: gets its name and its values as a JSON list, and returns a JSON object `{"ok": value}` or
/// `{"error": "why"}` (or null for nil). The returned string must stay valid until the callback is called again.
pub type HsNative = extern "C" fn(user: *mut c_void, name: *const c_char, args_json: *const c_char) -> *const c_char;

pub struct HsEngine {
    engine: Engine,
    out: CString,
}

/// # Safety
/// `name` must be null or a null-terminated string.
#[no_mangle]
pub unsafe extern "C" fn hs_new(name: *const c_char) -> *mut HsEngine {
    let name = text(name).to_string();
    catch_unwind(|| Box::into_raw(Box::new(HsEngine { engine: Engine::new(&name), out: CString::default() }))).unwrap_or(ptr::null_mut())
}

/// # Safety
/// `e` must come from `hs_new` and not be used again.
#[no_mangle]
pub unsafe extern "C" fn hs_free(e: *mut HsEngine) {
    if !e.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(e) })));
    }
}

struct UserPtr(*mut c_void);

/// Gives scripts a game function. Takes effect at the next `hs_load`.
///
/// # Safety
/// `e` must come from `hs_new`; `user` must stay valid while the engine lives.
#[no_mangle]
pub unsafe extern "C" fn hs_register(e: *mut HsEngine, name: *const c_char, f: HsNative, user: *mut c_void) {
    let Some(e) = (unsafe { e.as_mut() }) else { return };
    let name = text(name).to_string();
    let user = UserPtr(user);
    let cname = CString::new(name.clone()).unwrap_or_default();
    e.engine.register(&name, move |args| {
        let json = Json::Array(args.iter().map(to_json).collect()).to_string();
        let cargs = CString::new(json).unwrap_or_default();
        let back = f(user.0, cname.as_ptr(), cargs.as_ptr());
        if back.is_null() {
            return Ok(Value::Nil);
        }
        let s = unsafe { CStr::from_ptr(back) }.to_string_lossy().into_owned();
        match serde_json::from_str::<Json>(&s) {
            Ok(Json::Object(o)) if o.contains_key("error") => Err(o["error"].as_str().unwrap_or("the game said no").to_string()),
            Ok(Json::Object(o)) if o.contains_key("ok") => Ok(from_json(&o["ok"])),
            Ok(other) => Ok(from_json(&other)),
            Err(_) => Ok(Value::text(&s)),
        }
    });
}

fn script_call(e: *mut HsEngine, f: impl FnOnce(&mut Engine) -> Result<(), String>) -> *const c_char {
    let Some(e) = (unsafe { e.as_mut() }) else { return ptr::null() };
    match catch_unwind(AssertUnwindSafe(|| f(&mut e.engine))) {
        Ok(Ok(())) => ptr::null(),
        Ok(Err(m)) => keep(&mut e.out, &m),
        Err(_) => keep(&mut e.out, "Hearth Script stopped unexpectedly (this is a bug in Hearth Engine)"),
    }
}

/// Loads or reloads a script. Returns null when it worked, or the mistake (the last good version keeps running).
///
/// # Safety
/// `e` must come from `hs_new`.
#[no_mangle]
pub unsafe extern "C" fn hs_load(e: *mut HsEngine, source: *const c_char) -> *const c_char {
    let src = text(source).to_string();
    script_call(e, |en| en.load(&src).map_err(|x| x.to_string()))
}

/// Runs every `when event(…)` with these values (a JSON list). Null when it worked, or the first mistake.
///
/// # Safety
/// `e` must come from `hs_new`.
#[no_mangle]
pub unsafe extern "C" fn hs_fire(e: *mut HsEngine, event: *const c_char, args_json: *const c_char) -> *const c_char {
    let (ev, args) = (text(event).to_string(), args_from_json(text(args_json)));
    script_call(e, |en| en.fire(&ev, &args).map_err(|x| x.to_string()))
}

/// # Safety
/// `e` must come from `hs_new`.
#[no_mangle]
pub unsafe extern "C" fn hs_listens(e: *const HsEngine, event: *const c_char) -> bool {
    unsafe { e.as_ref() }.map(|e| e.engine.listens(text(event))).unwrap_or(false)
}

/// What `say` wrote since last asked, one line each (empty when nothing).
///
/// # Safety
/// `e` must come from `hs_new`.
#[no_mangle]
pub unsafe extern "C" fn hs_take_output(e: *mut HsEngine) -> *const c_char {
    let Some(e) = (unsafe { e.as_mut() }) else { return ptr::null() };
    let lines = e.engine.take_output().join("\n");
    keep(&mut e.out, &lines)
}

// ---------------------------------------------------------------- Hearth Detail

pub struct HdDetail {
    pub detail: Detail,
    tangents: Vec<[f32; 4]>,
    selected: Vec<u32>,
    chosen: Vec<u32>,
    error: CString,
}

thread_local! {
    static LAST_ERROR: std::cell::RefCell<CString> = std::cell::RefCell::new(CString::default());
}

fn set_error(m: &str) {
    LAST_ERROR.with(|e| *e.borrow_mut() = CString::new(m.replace('\0', " ")).unwrap_or_default());
}

/// Why the last `hd_open` failed.
#[no_mangle]
pub extern "C" fn hd_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}

/// Tangents from the uvs, for normal maps (xyz, and w = which way the bitangent goes).
fn tangents(d: &Detail) -> Vec<[f32; 4]> {
    let n = d.vertices.len();
    let mut t = vec![[0f32; 3]; n];
    let mut b = vec![[0f32; 3]; n];
    for tri in d.indices.as_chunks::<3>().0 {
        let [i0, i1, i2] = tri.map(|i| i as usize);
        let (v0, v1, v2) = (&d.vertices[i0], &d.vertices[i1], &d.vertices[i2]);
        let e1 = [v1.pos[0] - v0.pos[0], v1.pos[1] - v0.pos[1], v1.pos[2] - v0.pos[2]];
        let e2 = [v2.pos[0] - v0.pos[0], v2.pos[1] - v0.pos[1], v2.pos[2] - v0.pos[2]];
        let (du1, dv1) = (v1.uv[0] - v0.uv[0], v1.uv[1] - v0.uv[1]);
        let (du2, dv2) = (v2.uv[0] - v0.uv[0], v2.uv[1] - v0.uv[1]);
        let det = du1 * dv2 - du2 * dv1;
        if det.abs() < 1e-12 {
            continue;
        }
        let r = 1.0 / det;
        let tt = [(e1[0] * dv2 - e2[0] * dv1) * r, (e1[1] * dv2 - e2[1] * dv1) * r, (e1[2] * dv2 - e2[2] * dv1) * r];
        let bb = [(e2[0] * du1 - e1[0] * du2) * r, (e2[1] * du1 - e1[1] * du2) * r, (e2[2] * du1 - e1[2] * du2) * r];
        for i in [i0, i1, i2] {
            t[i] = [t[i][0] + tt[0], t[i][1] + tt[1], t[i][2] + tt[2]];
            b[i] = [b[i][0] + bb[0], b[i][1] + bb[1], b[i][2] + bb[2]];
        }
    }
    d.vertices
        .iter()
        .zip(t.iter().zip(&b))
        .map(|(v, (t, b))| {
            let nrm = v.normal;
            // Gram-Schmidt against the normal; any direction across it when the uvs give none.
            let d = t[0] * nrm[0] + t[1] * nrm[1] + t[2] * nrm[2];
            let mut o = [t[0] - nrm[0] * d, t[1] - nrm[1] * d, t[2] - nrm[2] * d];
            let mut l = (o[0] * o[0] + o[1] * o[1] + o[2] * o[2]).sqrt();
            if l < 1e-8 {
                o = if nrm[0].abs() < 0.9 { [0.0, nrm[2], -nrm[1]] } else { [-nrm[2], 0.0, nrm[0]] };
                l = (o[0] * o[0] + o[1] * o[1] + o[2] * o[2]).sqrt().max(1e-8);
            }
            let o = [o[0] / l, o[1] / l, o[2] / l];
            let c = [nrm[1] * o[2] - nrm[2] * o[1], nrm[2] * o[0] - nrm[0] * o[2], nrm[0] * o[1] - nrm[1] * o[0]];
            let w = if c[0] * b[0] + c[1] * b[1] + c[2] * b[2] < 0.0 { -1.0 } else { 1.0 };
            [o[0], o[1], o[2], w]
        })
        .collect()
}

fn open(path: &str) -> Result<Detail, String> {
    let p = std::path::Path::new(path);
    if p.extension().and_then(|e| e.to_str()).map(|e| e.eq_ignore_ascii_case("hdetail")).unwrap_or(false) {
        let bytes = std::fs::read(p).map_err(|e| format!("can't open {path}: {e}"))?;
        return format::read(&bytes);
    }
    // A model: use the built file next to it when it's newer, otherwise build and save it for next time.
    let built = p.with_extension("hdetail");
    let newer = |a: &std::path::Path, b: &std::path::Path| -> bool {
        let m = |x: &std::path::Path| std::fs::metadata(x).and_then(|m| m.modified()).ok();
        matches!((m(a), m(b)), (Some(x), Some(y)) if x >= y)
    };
    if newer(&built, p) {
        if let Ok(d) = std::fs::read(&built).map_err(|e| e.to_string()).and_then(|b| format::read(&b)) {
            return Ok(d);
        }
    }
    let d = build(&load::load(p)?)?;
    let _ = std::fs::write(&built, format::write(&d)); // a cache: fine if the folder is read-only
    Ok(d)
}

/// Opens a .glb, .obj or .hdetail file (building the detail levels when needed). Null on failure: see
/// `hd_last_error`.
///
/// # Safety
/// `path` must be a null-terminated string.
#[no_mangle]
pub unsafe extern "C" fn hd_open(path: *const c_char) -> *mut HdDetail {
    let path = text(path).to_string();
    match catch_unwind(|| open(&path)) {
        Ok(Ok(detail)) => {
            let tangents = tangents(&detail);
            Box::into_raw(Box::new(HdDetail { detail, tangents, selected: Vec::new(), chosen: Vec::new(), error: CString::default() }))
        }
        Ok(Err(m)) => {
            set_error(&m);
            ptr::null_mut()
        }
        Err(_) => {
            set_error("building the detail levels stopped unexpectedly (this is a bug in Hearth Engine)");
            ptr::null_mut()
        }
    }
}

/// # Safety
/// `d` must come from `hd_open` and not be used again.
#[no_mangle]
pub unsafe extern "C" fn hd_free(d: *mut HdDetail) {
    if !d.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| drop(unsafe { Box::from_raw(d) })));
    }
}

/// Every vertex, 12 floats each: position xyz, normal xyz, uv, tangent xyzw. Returns the count.
///
/// # Safety
/// `d` must come from `hd_open`; `out` must hold 12 floats per vertex (or be null to just count).
#[no_mangle]
pub unsafe extern "C" fn hd_vertices(d: *const HdDetail, out: *mut f32) -> u32 {
    let Some(d) = (unsafe { d.as_ref() }) else { return 0 };
    if !out.is_null() {
        let out = unsafe { std::slice::from_raw_parts_mut(out, d.detail.vertices.len() * 12) };
        for (k, (v, t)) in d.detail.vertices.iter().zip(&d.tangents).enumerate() {
            out[k * 12..k * 12 + 12].copy_from_slice(&[v.pos[0], v.pos[1], v.pos[2], v.normal[0], v.normal[1], v.normal[2], v.uv[0], v.uv[1], t[0], t[1], t[2], t[3]]);
        }
    }
    d.detail.vertices.len() as u32
}

/// The model's box: min xyz then max xyz.
///
/// # Safety
/// `d` must come from `hd_open`; `out` must hold 6 floats.
#[no_mangle]
pub unsafe extern "C" fn hd_bounds(d: *const HdDetail, out: *mut f32) {
    let (Some(d), false) = (unsafe { d.as_ref() }, out.is_null()) else { return };
    let mut lo = [f32::MAX; 3];
    let mut hi = [f32::MIN; 3];
    for v in &d.detail.vertices {
        lo = [lo[0].min(v.pos[0]), lo[1].min(v.pos[1]), lo[2].min(v.pos[2])];
        hi = [hi[0].max(v.pos[0]), hi[1].max(v.pos[1]), hi[2].max(v.pos[2])];
    }
    unsafe { std::slice::from_raw_parts_mut(out, 6) }.copy_from_slice(&[lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]]);
}

/// How big an error of `error` model units looks from the camera, in pixels.
fn looks(error: f32, s: &hearth_detail::math::Sphere, camera: [f32; 3], pixels_per_unit: f32) -> f32 {
    if error.is_infinite() {
        return f32::INFINITY;
    }
    let d = hearth_detail::math::dist2(s.center, camera).sqrt() - s.radius;
    error / d.max(1e-3) * pixels_per_unit
}

/// The pieces to draw from `camera` (in the model's own space): those whose error looks at most `max_pixels` and
/// whose simpler version's error doesn't. `pixels_per_unit` is how many pixels one unit 1 unit away covers (screen
/// height / (2 tan(fov/2)), divided by the model's scale). Returns true when the choice changed since last time; the
/// triangles are then in `hd_selected`.
///
/// # Safety
/// `d` must come from `hd_open`.
#[no_mangle]
pub unsafe extern "C" fn hd_select(d: *mut HdDetail, cx: f32, cy: f32, cz: f32, pixels_per_unit: f32, max_pixels: f32) -> bool {
    let Some(d) = (unsafe { d.as_mut() }) else { return false };
    let cam = [cx, cy, cz];
    let chosen: Vec<u32> = d
        .detail
        .clusters
        .iter()
        .enumerate()
        .filter(|(_, c)| looks(c.error, &c.lod_bounds, cam, pixels_per_unit) <= max_pixels && looks(c.parent_error, &c.parent_bounds, cam, pixels_per_unit) > max_pixels)
        .map(|(i, _)| i as u32)
        .collect();
    if chosen == d.chosen && !d.selected.is_empty() {
        return false;
    }
    d.selected.clear();
    for &c in &chosen {
        let c = &d.detail.clusters[c as usize];
        d.selected.extend_from_slice(&d.detail.indices[c.first_index as usize..(c.first_index + c.index_count) as usize]);
    }
    d.chosen = chosen;
    true
}

/// The triangles chosen by the last `hd_select` (indices into the vertices), and how many indices there are.
///
/// # Safety
/// `d` must come from `hd_open`; `count` must be writable.
#[no_mangle]
pub unsafe extern "C" fn hd_selected(d: *const HdDetail, count: *mut u32) -> *const u32 {
    let Some(d) = (unsafe { d.as_ref() }) else { return ptr::null() };
    if let Some(c) = unsafe { count.as_mut() } {
        *c = d.selected.len() as u32;
    }
    d.selected.as_ptr()
}

/// Facts for the debug view: total triangles, pieces, levels, and triangles drawn now.
///
/// # Safety
/// `d` must come from `hd_open`.
#[no_mangle]
pub unsafe extern "C" fn hd_describe(d: *mut HdDetail) -> *const c_char {
    let Some(d) = (unsafe { d.as_mut() }) else { return ptr::null() };
    let full = d.detail.clusters.iter().filter(|c| c.level == 0).map(|c| c.index_count / 3).sum::<u32>();
    let s = format!("{full} triangles, {} pieces over {} levels; drawing {} now", d.detail.clusters.len(), d.detail.levels, d.selected.len() / 3);
    keep(&mut d.error, &s)
}

// ---------------------------------------------------------------- Hearth Texture (the asset pipeline's compressor)

/// Bytes one mip takes compressed (`bc`: 1 BC1, 2 BC1a, 3 BC3, 4 BC4, 5 BC5). 0 for an unknown format.
#[no_mangle]
pub extern "C" fn ht_compressed_size(bc: u32, width: u32, height: u32) -> u64 {
    hearth_texture::Bc::from_code(bc).map(|b| b.compressed_size(width as usize, height as usize) as u64).unwrap_or(0)
}

/// Compresses one mip. `source`: 1 RGBA8, 2 RGBX8, 3 R8, 4 RGBA32F, rows `pitch` bytes apart. `quality` 0-3. Writes
/// `ht_compressed_size` bytes to `out`. False when something doesn't fit.
///
/// # Safety
/// `src` must hold `height` rows of `pitch` bytes; `out` must hold `out_len` bytes.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ht_compress(
    bc: u32,
    source: u32,
    src: *const u8,
    width: u32,
    height: u32,
    pitch: u32,
    quality: u32,
    weights: *const f32,
    out: *mut u8,
    out_len: u64,
) -> bool {
    let (Some(bc), Some(source)) = (hearth_texture::Bc::from_code(bc), hearth_texture::Source::from_code(source)) else { return false };
    if src.is_null() || out.is_null() || width == 0 || height == 0 {
        return false;
    }
    let (w, h) = (width as usize, height as usize);
    let need = bc.compressed_size(w, h);
    if (out_len as usize) < need {
        return false;
    }
    let len = pitch as usize * (h - 1) + w * source.bytes_per_pixel();
    let input = unsafe { std::slice::from_raw_parts(src, len) };
    let weights = if weights.is_null() { [1.0, 1.0, 1.0] } else { unsafe { [*weights, *weights.add(1), *weights.add(2)] } };
    let result = catch_unwind(|| {
        let rgba = hearth_texture::to_rgba8(input, source, w, h, pitch as usize, bc)?;
        Ok::<Vec<u8>, String>(hearth_texture::compress(&rgba, w, h, bc, quality, weights))
    });
    match result {
        Ok(Ok(data)) => {
            unsafe { std::slice::from_raw_parts_mut(out, need) }.copy_from_slice(&data);
            true
        }
        _ => false,
    }
}

/// Decompresses one mip into rows `out_pitch` bytes apart: RGBA8, or RGBA32F (0 to 1) when `as_float`.
///
/// # Safety
/// `data` must hold `data_len` bytes; `out` must hold `height` rows of `out_pitch` bytes.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn ht_decompress(bc: u32, data: *const u8, data_len: u64, width: u32, height: u32, out: *mut u8, out_pitch: u32, as_float: bool) -> bool {
    let Some(bc) = hearth_texture::Bc::from_code(bc) else { return false };
    if data.is_null() || out.is_null() || width == 0 || height == 0 {
        return false;
    }
    let (w, h, pitch) = (width as usize, height as usize, out_pitch as usize);
    let bpp = if as_float { 16 } else { 4 };
    if pitch < w * bpp {
        return false;
    }
    let input = unsafe { std::slice::from_raw_parts(data, data_len as usize) };
    let Ok(Ok(rgba)) = catch_unwind(|| hearth_texture::decompress(input, w, h, bc)) else { return false };
    let output = unsafe { std::slice::from_raw_parts_mut(out, pitch * (h - 1) + w * bpp) };
    for y in 0..h {
        let row = &rgba[y * w * 4..(y + 1) * w * 4];
        let dst = &mut output[y * pitch..y * pitch + w * bpp];
        if as_float {
            for (d, s) in dst.chunks_exact_mut(16).zip(row.chunks_exact(4)) {
                for (k, v) in s.iter().enumerate() {
                    d[k * 4..k * 4 + 4].copy_from_slice(&(*v as f32 / 255.0).to_le_bytes());
                }
            }
        } else {
            dst.copy_from_slice(row);
        }
    }
    true
}
