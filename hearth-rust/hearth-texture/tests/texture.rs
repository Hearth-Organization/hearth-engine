use hearth_texture::*;

/// A smooth, colourful test picture with a gradient alpha.
fn picture(w: usize, h: usize) -> Vec<u8> {
    let mut v = Vec::with_capacity(w * h * 4);
    for y in 0..h {
        for x in 0..w {
            let (fx, fy) = (x as f32 / w as f32, y as f32 / h as f32);
            v.extend([(fx * 255.0) as u8, (fy * 255.0) as u8, (((fx + fy) * 3.0).sin() * 127.0 + 128.0) as u8, (fx * 255.0) as u8]);
        }
    }
    v
}

#[test]
fn every_format_round_trips_with_good_quality() {
    let (w, h) = (64, 48);
    let src = picture(w, h);
    for (bc, channels, at_least) in [(Bc::Bc1, vec![0, 1, 2], 32.0), (Bc::Bc3, vec![0, 1, 2, 3], 32.0), (Bc::Bc4, vec![0], 40.0), (Bc::Bc5, vec![0, 1], 40.0)] {
        let rgba = to_rgba8(&src, Source::Rgba8, w, h, w * 4, bc).unwrap();
        let data = compress(&rgba, w, h, bc, 2, [1.0, 1.0, 1.0]);
        assert_eq!(data.len(), bc.compressed_size(w, h));
        let back = decompress(&data, w, h, bc).unwrap();
        let q = psnr(&rgba, &back, &channels);
        assert!(q >= at_least, "{bc:?}: {q:.1} dB");
    }
}

#[test]
fn slower_is_at_least_as_good_and_odd_sizes_work() {
    let (w, h) = (37, 13); // not a multiple of 4
    let rgba = to_rgba8(&picture(w, h), Source::Rgba8, w, h, w * 4, Bc::Bc1).unwrap();
    let fast = decompress(&compress(&rgba, w, h, Bc::Bc1, 0, [1.0; 3]), w, h, Bc::Bc1).unwrap();
    let slow = decompress(&compress(&rgba, w, h, Bc::Bc1, 3, [1.0; 3]), w, h, Bc::Bc1).unwrap();
    assert!(psnr(&rgba, &slow, &[0, 1, 2]) >= psnr(&rgba, &fast, &[0, 1, 2]) - 0.01);
    for (w, h) in [(1, 1), (2, 2), (3, 5)] {
        let rgba = to_rgba8(&picture(w, h), Source::Rgba8, w, h, w * 4, Bc::Bc5).unwrap();
        let d = compress(&rgba, w, h, Bc::Bc5, 2, [1.0; 3]);
        assert_eq!(d.len(), w.div_ceil(4) * h.div_ceil(4) * 16, "whole blocks, even for tiny mips");
        decompress(&d, w, h, Bc::Bc5).unwrap();
    }
}

#[test]
fn bc1_ignores_alpha_and_bc1a_cuts_it_out() {
    let (w, h) = (8, 8);
    let mut src = picture(w, h);
    for p in src.chunks_exact_mut(4) {
        p[3] = if p[0] > 128 { 255 } else { 0 };
    }
    let solid = decompress(&compress(&to_rgba8(&src, Source::Rgba8, w, h, w * 4, Bc::Bc1).unwrap(), w, h, Bc::Bc1, 2, [1.0; 3]), w, h, Bc::Bc1).unwrap();
    assert!(solid.chunks_exact(4).all(|p| p[3] == 255));
    let cut = decompress(&compress(&to_rgba8(&src, Source::Rgba8, w, h, w * 4, Bc::Bc1a).unwrap(), w, h, Bc::Bc1a, 2, [1.0; 3]), w, h, Bc::Bc1a).unwrap();
    for (a, b) in src.chunks_exact(4).zip(cut.chunks_exact(4)) {
        assert_eq!(a[3] == 0, b[3] == 0, "see-through pixels stay see-through");
    }
}

#[test]
fn sources_with_pitch_floats_and_one_channel() {
    let (w, h) = (4, 4);
    // Float RGBA with padding at the end of each row.
    let pitch = w * 16 + 8;
    let mut f = vec![0u8; pitch * h];
    for y in 0..h {
        for x in 0..w {
            for (k, v) in [0.5f32, 1.0, -1.0, 2.0].iter().enumerate() {
                f[y * pitch + x * 16 + k * 4..y * pitch + x * 16 + k * 4 + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
    }
    let rgba = to_rgba8(&f, Source::Rgba32F, w, h, pitch, Bc::Bc5).unwrap();
    assert_eq!(&rgba[..4], &[128, 255, 0, 255], "floats are clamped to 0..1");
    let r8 = to_rgba8(&[7u8; 16], Source::R8, w, h, w, Bc::Bc4).unwrap();
    assert_eq!(&r8[..4], &[7, 7, 7, 255]);
    assert!(to_rgba8(&[0u8; 10], Source::Rgba8, w, h, w * 4, Bc::Bc1).is_err());
    assert!(decompress(&[0u8; 4], 4, 4, Bc::Bc1).is_err());
}
