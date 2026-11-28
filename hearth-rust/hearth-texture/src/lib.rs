//! Texture compression for the asset pipeline: the BC formats the engine compressed with libsquish (through
//! CryTextureSquisher), now in Rust through texpresso, the same algorithms with the same quality.
//!
//! In: rows of pixels (8-bit RGBA, 8-bit single channel, or 32-bit float RGBA), with any row pitch.
//! Out: tightly packed blocks, the way the engine's image objects store compressed mips.

use texpresso::{Algorithm, Format, Params};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bc {
    /// Colour, no alpha.
    Bc1,
    /// Colour with cut-out alpha (alpha under 128 is fully see-through).
    Bc1a,
    /// Colour with smooth alpha.
    Bc3,
    /// One channel (red).
    Bc4,
    /// Two channels (red, green): normal maps.
    Bc5,
}

impl Bc {
    pub fn from_code(c: u32) -> Option<Bc> {
        Some(match c {
            1 => Bc::Bc1,
            2 => Bc::Bc1a,
            3 => Bc::Bc3,
            4 => Bc::Bc4,
            5 => Bc::Bc5,
            _ => return None,
        })
    }
    fn format(self) -> Format {
        match self {
            Bc::Bc1 | Bc::Bc1a => Format::Bc1,
            Bc::Bc3 => Format::Bc3,
            Bc::Bc4 => Format::Bc4,
            Bc::Bc5 => Format::Bc5,
        }
    }
    pub fn compressed_size(self, width: usize, height: usize) -> usize {
        self.format().compressed_size(width, height)
    }
}

/// How the source pixels are laid out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Source {
    Rgba8,
    /// RGB with an unused fourth byte (treated as opaque).
    Rgbx8,
    R8,
    Rgba32F,
}

impl Source {
    pub fn from_code(c: u32) -> Option<Source> {
        Some(match c {
            1 => Source::Rgba8,
            2 => Source::Rgbx8,
            3 => Source::R8,
            4 => Source::Rgba32F,
            _ => return None,
        })
    }
    pub fn bytes_per_pixel(self) -> usize {
        match self {
            Source::Rgba8 | Source::Rgbx8 => 4,
            Source::R8 => 1,
            Source::Rgba32F => 16,
        }
    }
}

/// 0 preview, 1 fast, 2 normal, 3 slow: the engine's quality levels.
pub fn algorithm(quality: u32) -> Algorithm {
    match quality {
        0 | 1 => Algorithm::RangeFit,
        2 => Algorithm::ClusterFit,
        _ => Algorithm::IterativeClusterFit,
    }
}

fn to_byte(x: f32) -> u8 {
    (x.clamp(0.0, 1.0) * 255.0 + 0.5) as u8
}

/// The pixels as tightly packed RGBA8 (what the compressor reads).
pub fn to_rgba8(src: &[u8], source: Source, width: usize, height: usize, pitch: usize, bc: Bc) -> Result<Vec<u8>, String> {
    let bpp = source.bytes_per_pixel();
    if pitch < width * bpp || src.len() < pitch * height.saturating_sub(1) + width * bpp {
        return Err(format!("the image data is too small for {width} x {height}"));
    }
    let mut out = vec![0u8; width * height * 4];
    for y in 0..height {
        let row = &src[y * pitch..y * pitch + width * bpp];
        let dst = &mut out[y * width * 4..(y + 1) * width * 4];
        for (x, d) in dst.chunks_exact_mut(4).enumerate() {
            let p = &row[x * bpp..(x + 1) * bpp];
            let px = match source {
                Source::Rgba8 => [p[0], p[1], p[2], p[3]],
                Source::Rgbx8 => [p[0], p[1], p[2], 255],
                Source::R8 => [p[0], p[0], p[0], 255],
                Source::Rgba32F => {
                    let f = |k: usize| f32::from_le_bytes([p[k * 4], p[k * 4 + 1], p[k * 4 + 2], p[k * 4 + 3]]);
                    [to_byte(f(0)), to_byte(f(1)), to_byte(f(2)), to_byte(f(3))]
                }
            };
            d.copy_from_slice(&px);
            // Plain BC1 has no alpha: every pixel counts as solid.
            if bc == Bc::Bc1 {
                d[3] = 255;
            }
        }
    }
    Ok(out)
}

/// Compresses one mip. `weights`: how much red, green and blue matter (the engine's rgbWeight).
pub fn compress(rgba8: &[u8], width: usize, height: usize, bc: Bc, quality: u32, weights: [f32; 3]) -> Vec<u8> {
    let params = Params { algorithm: algorithm(quality), weights, weigh_colour_by_alpha: bc == Bc::Bc3 };
    let mut out = vec![0u8; bc.compressed_size(width, height)];
    bc.format().compress(rgba8, width, height, params, &mut out);
    out
}

/// Decompresses one mip into tightly packed RGBA8 (BC4: red only; BC5: red and green; the rest 0, alpha 255).
pub fn decompress(data: &[u8], width: usize, height: usize, bc: Bc) -> Result<Vec<u8>, String> {
    if data.len() < bc.compressed_size(width, height) {
        return Err(format!("the compressed data is too small for {width} x {height}"));
    }
    let mut out = vec![0u8; width * height * 4];
    bc.format().decompress(data, width, height, &mut out);
    if matches!(bc, Bc::Bc4 | Bc::Bc5) {
        for p in out.chunks_exact_mut(4) {
            if bc == Bc::Bc4 {
                p[1] = 0;
            }
            p[2] = 0;
            p[3] = 255;
        }
    }
    Ok(out)
}

/// Peak signal-to-noise ratio between two RGBA8 images over the given channels (higher is closer; 40+ is very
/// good for BC formats).
pub fn psnr(a: &[u8], b: &[u8], channels: &[usize]) -> f64 {
    let mut sum = 0f64;
    let mut n = 0usize;
    for (pa, pb) in a.chunks_exact(4).zip(b.chunks_exact(4)) {
        for &c in channels {
            let d = pa[c] as f64 - pb[c] as f64;
            sum += d * d;
            n += 1;
        }
    }
    if sum == 0.0 {
        return f64::INFINITY;
    }
    10.0 * (255.0f64 * 255.0 / (sum / n as f64)).log10()
}
