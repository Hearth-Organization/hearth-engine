use hearth_texture::mips::*;

fn image(w: usize, h: usize, f: impl Fn(usize, usize) -> [f32; 4]) -> Image {
    Image { width: w, height: h, pixels: (0..h).flat_map(|y| (0..w).map(move |x| (x, y))).map(|(x, y)| f(x, y)).collect() }
}

#[test]
fn the_chain_halves_down_to_one_pixel_for_any_size() {
    for (w, h, sizes) in [(16, 16, vec![(16, 16), (8, 8), (4, 4), (2, 2), (1, 1)]), (10, 3, vec![(10, 3), (5, 1), (2, 1), (1, 1)])] {
        let c = chain(&image(w, h, |_, _| [0.5, 0.5, 0.5, 1.0]), &Options::default());
        assert_eq!(c.iter().map(|i| (i.width, i.height)).collect::<Vec<_>>(), sizes);
    }
    let c = chain(&image(16, 16, |_, _| [0.5; 4]), &Options { smallest: 4, ..Options::default() });
    assert_eq!(c.last().map(|i| i.width), Some(4));
}

#[test]
fn a_flat_colour_stays_the_same_with_either_filter() {
    for filter in [Filter::Box, Filter::Kaiser] {
        let c = chain(&image(32, 32, |_, _| [0.2, 0.6, 0.9, 0.7]), &Options { filter, ..Options::default() });
        for level in &c {
            for p in &level.pixels {
                assert!((p[0] - 0.2).abs() < 1e-3 && (p[1] - 0.6).abs() < 1e-3 && (p[2] - 0.9).abs() < 1e-3 && (p[3] - 0.7).abs() < 1e-3, "{filter:?} {p:?}");
            }
        }
    }
}

#[test]
fn colour_is_averaged_as_light() {
    let checker = image(8, 8, |x, y| if (x + y) % 2 == 0 { [1.0, 1.0, 1.0, 1.0] } else { [0.0, 0.0, 0.0, 1.0] });
    let c = chain(&checker, &Options { filter: Filter::Box, ..Options::default() });
    let grey = c[1].pixels[0][0];
    assert!((grey - linear_to_srgb(0.5)).abs() < 0.01, "black and white make mid-grey light (about 0.735), got {grey}");
    let plain = chain(&checker, &Options { filter: Filter::Box, srgb: false, ..Options::default() });
    assert!((plain[1].pixels[0][0] - 0.5).abs() < 0.01);
}

#[test]
fn cut_outs_keep_their_coverage_and_normals_stay_unit() {
    // Thin solid strands on a see-through background, like leaves or a fence.
    let fence = image(64, 64, |x, _| if x % 8 < 2 { [0.3, 0.6, 0.2, 1.0] } else { [0.3, 0.6, 0.2, 0.0] });
    let want = coverage(&fence, 0.5);
    let c = chain(&fence, &Options { alpha_cutoff: Some(0.5), ..Options::default() });
    for level in c.iter().take(4) {
        assert!((coverage(level, 0.5) - want).abs() < 0.08, "{}x{}: {} vs {want}", level.width, level.height, coverage(level, 0.5));
    }
    let bumps = image(32, 32, |x, y| {
        let (nx, ny) = (((x as f32) * 0.7).sin() * 0.5, ((y as f32) * 0.4).cos() * 0.5);
        let nz = (1.0 - nx * nx - ny * ny).sqrt();
        [nx * 0.5 + 0.5, ny * 0.5 + 0.5, nz * 0.5 + 0.5, 1.0]
    });
    for level in chain(&bumps, &Options { normal_map: true, ..Options::default() }).iter().skip(1) {
        for p in &level.pixels {
            let v = [p[0] * 2.0 - 1.0, p[1] * 2.0 - 1.0, p[2] * 2.0 - 1.0];
            let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
            assert!((l - 1.0).abs() < 0.01, "length {l}");
        }
    }
}
