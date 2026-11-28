//! Mipmaps: each smaller copy of a texture, made the right way.
//!
//! - Colour is averaged as light, not as numbers (sRGB textures are turned linear, filtered, turned back), so a
//!   black-and-white checker turns mid-grey as it should, not too dark.
//! - Cut-out textures (leaves, fences) keep the same share of solid pixels at every size, so they don't thin out
//!   and vanish in the distance.
//! - Normal maps stay normal (each vector made length 1 again).
//! - The filter is a Kaiser-windowed sinc (sharp without ringing much), or a plain box for speed.

/// An RGBA image in floats, row by row.
#[derive(Clone, Debug, PartialEq)]
pub struct Image {
    pub width: usize,
    pub height: usize,
    pub pixels: Vec<[f32; 4]>,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Filter {
    Box,
    Kaiser,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Options {
    pub filter: Filter,
    pub srgb: bool,
    pub normal_map: bool,
    /// Keep the share of pixels with alpha at or above this the same at every size (cut-outs); None: leave alpha.
    pub alpha_cutoff: Option<f32>,
    /// Stop at this size (1 = all the way down).
    pub smallest: usize,
}

impl Default for Options {
    fn default() -> Options {
        Options { filter: Filter::Kaiser, srgb: true, normal_map: false, alpha_cutoff: None, smallest: 1 }
    }
}

pub fn srgb_to_linear(c: f32) -> f32 {
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

pub fn linear_to_srgb(c: f32) -> f32 {
    if c <= 0.0031308 {
        c * 12.92
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    }
}

fn bessel_i0(x: f32) -> f32 {
    let mut sum = 1.0f32;
    let mut term = 1.0f32;
    let y = x * x / 4.0;
    for k in 1..20 {
        term *= y / (k * k) as f32;
        sum += term;
    }
    sum
}

/// Weights for one output pixel from the source pixels around it (halving: centre between two source pixels).
fn taps(filter: Filter, out_index: usize, src_len: usize) -> Vec<(usize, f32)> {
    let scale = src_len as f32 / ((src_len / 2).max(1)) as f32;
    let centre = (out_index as f32 + 0.5) * scale - 0.5;
    let mut t: Vec<(usize, f32)> = Vec::new();
    match filter {
        Filter::Box => {
            let start = (out_index as f32 * scale).floor() as usize;
            let end = (((out_index + 1) as f32 * scale).ceil() as usize).min(src_len).max(start + 1);
            for i in start..end {
                t.push((i, 1.0));
            }
        }
        Filter::Kaiser => {
            let radius = 3.0 * scale / 2.0;
            let alpha = 4.0;
            let lo = (centre - radius).floor() as isize;
            let hi = (centre + radius).ceil() as isize;
            for i in lo..=hi {
                let x = (i as f32 - centre) / scale;
                let r = (i as f32 - centre).abs() / radius;
                if r >= 1.0 {
                    continue;
                }
                let sinc = if x.abs() < 1e-6 { 1.0 } else { (std::f32::consts::PI * x).sin() / (std::f32::consts::PI * x) };
                let window = bessel_i0(alpha * (1.0 - r * r).sqrt()) / bessel_i0(alpha);
                // Edges repeat the last pixel.
                let idx = i.clamp(0, src_len as isize - 1) as usize;
                t.push((idx, sinc * window));
            }
        }
    }
    let total: f32 = t.iter().map(|x| x.1).sum();
    for x in &mut t {
        x.1 /= total;
    }
    t
}

fn halve(img: &Image, filter: Filter) -> Image {
    let (w, h) = ((img.width / 2).max(1), (img.height / 2).max(1));
    // Rows first, then columns.
    let mut mid = vec![[0f32; 4]; w * img.height];
    let xt: Vec<Vec<(usize, f32)>> = (0..w).map(|x| if img.width == 1 { vec![(0, 1.0)] } else { taps(filter, x, img.width) }).collect();
    for y in 0..img.height {
        for (x, taps) in xt.iter().enumerate() {
            let mut a = [0f32; 4];
            for &(i, wt) in taps {
                let p = img.pixels[y * img.width + i];
                for (ak, pk) in a.iter_mut().zip(p) {
                    *ak += pk * wt;
                }
            }
            mid[y * w + x] = a;
        }
    }
    let yt: Vec<Vec<(usize, f32)>> = (0..h).map(|y| if img.height == 1 { vec![(0, 1.0)] } else { taps(filter, y, img.height) }).collect();
    let mut out = vec![[0f32; 4]; w * h];
    for (y, taps) in yt.iter().enumerate() {
        for x in 0..w {
            let mut a = [0f32; 4];
            for &(i, wt) in taps {
                let p = mid[i * w + x];
                for (ak, pk) in a.iter_mut().zip(p) {
                    *ak += pk * wt;
                }
            }
            out[y * w + x] = a;
        }
    }
    Image { width: w, height: h, pixels: out }
}

/// The share of pixels whose alpha is at or above `cut`.
pub fn coverage(img: &Image, cut: f32) -> f32 {
    img.pixels.iter().filter(|p| p[3] >= cut).count() as f32 / img.pixels.len().max(1) as f32
}

/// Scales alpha so that `coverage(img, cut)` matches `want` (a search over the scale).
fn keep_coverage(img: &mut Image, cut: f32, want: f32) {
    let (mut lo, mut hi) = (0.0f32, 8.0f32);
    for _ in 0..20 {
        let mid = (lo + hi) / 2.0;
        let c = img.pixels.iter().filter(|p| (p[3] * mid).min(1.0) >= cut).count() as f32 / img.pixels.len() as f32;
        if c < want {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    let s = (lo + hi) / 2.0;
    for p in &mut img.pixels {
        p[3] = (p[3] * s).min(1.0);
    }
}

/// The whole chain, the full size first.
pub fn chain(top: &Image, o: &Options) -> Vec<Image> {
    let encode = |img: &Image| -> Image {
        let mut i = img.clone();
        if o.srgb && !o.normal_map {
            for p in &mut i.pixels {
                for c in p.iter_mut().take(3) {
                    *c = srgb_to_linear(c.clamp(0.0, 1.0));
                }
            }
        }
        if o.normal_map {
            for p in &mut i.pixels {
                for c in p.iter_mut().take(3) {
                    *c = *c * 2.0 - 1.0;
                }
            }
        }
        i
    };
    let decode = |img: &Image| -> Image {
        let mut i = img.clone();
        for p in &mut i.pixels {
            if o.normal_map {
                let l = (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]).sqrt().max(1e-6);
                for c in p.iter_mut().take(3) {
                    *c = *c / l * 0.5 + 0.5;
                }
            } else if o.srgb {
                for c in p.iter_mut().take(3) {
                    *c = linear_to_srgb(c.clamp(0.0, 1.0));
                }
            }
            for c in p.iter_mut() {
                *c = c.clamp(0.0, 1.0);
            }
        }
        i
    };
    let want = o.alpha_cutoff.map(|cut| coverage(top, cut));
    let mut out = vec![top.clone()];
    let mut work = encode(top);
    let smallest = o.smallest.max(1);
    while (work.width > smallest || work.height > smallest) && (work.width > 1 || work.height > 1) {
        work = halve(&work, o.filter);
        let mut level = decode(&work);
        if let (Some(cut), Some(want)) = (o.alpha_cutoff, want) {
            keep_coverage(&mut level, cut, want);
        }
        out.push(level);
    }
    out
}
