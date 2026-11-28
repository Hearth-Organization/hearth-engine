use glam::{Mat4, Vec3};
use hearth_render::{save_png, shapes, Camera, Material, Object, PointLight, Renderer, Scene, Sky, Sun};

fn pixel(img: &[u8], w: u32, x: u32, y: u32) -> [u8; 4] {
    let i = ((y * w + x) * 4) as usize;
    [img[i], img[i + 1], img[i + 2], img[i + 3]]
}

fn bright(p: [u8; 4]) -> u32 {
    p[0] as u32 + p[1] as u32 + p[2] as u32
}

#[test]
fn a_lit_scene_renders_and_is_saved() {
    let (w, h) = (256, 192);
    let mut r = match Renderer::headless(w, h) {
        Ok(r) => r,
        Err(e) if std::env::var_os("HEARTH_NEED_GPU").is_none() => {
            eprintln!("skipped: {e}");
            return;
        }
        Err(e) => panic!("{e}"),
    };
    println!("drawing with {}", r.adapter_name);
    let ball = r.add_mesh(&shapes::sphere(32, 48)).unwrap();
    let floor = r.add_mesh(&shapes::plane(12.0)).unwrap();
    let block = r.add_mesh(&shapes::cube()).unwrap();
    let mut scene = Scene {
        camera: Camera { eye: Vec3::new(0.0, -5.0, 2.0), target: Vec3::new(0.0, 0.0, 0.6), ..Camera::default() },
        sun: Some(Sun { direction: Vec3::new(-0.4, 0.6, -1.0), color: [1.0, 0.95, 0.85], intensity: 3.0 }),
        ambient: [0.05, 0.05, 0.07],
        lights: vec![PointLight { position: Vec3::new(1.8, -1.0, 1.2), color: [1.0, 0.5, 0.2], intensity: 8.0, range: 6.0 }],
        clear: [0.02, 0.02, 0.03, 1.0],
        ..Scene::default()
    };
    scene.objects.push(Object { mesh: floor, transform: Mat4::IDENTITY, material: Material { color: [0.5, 0.5, 0.5, 1.0], roughness: 0.9, ..Material::default() } });
    scene.objects.push(Object { mesh: ball, transform: Mat4::from_translation(Vec3::new(0.0, 0.0, 1.0)), material: Material { color: [0.8, 0.1, 0.1, 1.0], roughness: 0.35, ..Material::default() } });
    scene.objects.push(Object {
        mesh: block,
        transform: Mat4::from_translation(Vec3::new(-2.0, 0.5, 0.5)) * Mat4::from_rotation_z(0.6),
        material: Material { color: [0.9, 0.75, 0.3, 1.0], metallic: 1.0, roughness: 0.25, ..Material::default() },
    });
    let img = r.render(&scene).unwrap();
    assert_eq!(img.len(), (w * h * 4) as usize);
    let out = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../target/renders/first-scene.png");
    save_png(&out, &img, w, h).unwrap();

    // The ball is in the middle and red; the top corner is the dark background.
    let ball_px = pixel(&img, w, w / 2, h / 2 - 10);
    let samples: Vec<String> = [(w / 2, h / 2 - 10), (w / 2, h / 4), (w / 2, 3 * h / 4), (w / 4, h / 2), (3 * w / 4, h / 2), (2, 2)]
        .iter()
        .map(|&(x, y)| format!("{x},{y}={:?}", pixel(&img, w, x, y)))
        .collect();
    assert!(ball_px[0] > ball_px[1] + 30 && ball_px[0] > 60, "the middle should be the red ball: {}", samples.join(" "));
    let sky = pixel(&img, w, 2, 2);
    // The background colour, linear (0.02, 0.02, 0.03), shown in sRGB: about (39, 39, 48).
    assert!((sky[0] as i32 - 39).abs() <= 3 && (sky[2] as i32 - 48).abs() <= 3, "the corner should be the background: {sky:?}");
    // Without the lights the ball goes dark.
    scene.sun = None;
    scene.lights.clear();
    let dark = r.render(&scene).unwrap();
    assert!(bright(pixel(&dark, w, w / 2, h / 2 - 10)) < bright(ball_px) / 2, "lights make the difference");
    save_png(&out.with_file_name("no-lights.png"), &dark, w, h).unwrap();
    // A thing using a mesh that isn't there is explained.
    scene.objects.push(Object { mesh: ball, transform: Mat4::IDENTITY, material: Material::default() });
    r.remove_mesh(ball);
    assert!(r.render(&scene).unwrap_err().contains("isn't on the graphics card"));
}

#[test]
fn bad_meshes_are_explained() {
    let Ok(mut r) = Renderer::headless(8, 8) else { return };
    let mut m = shapes::cube();
    m.indices.push(0);
    assert!(r.add_mesh(&m).unwrap_err().contains("whole triangles"));
    let mut m = shapes::cube();
    m.indices[0] = 999;
    assert!(r.add_mesh(&m).unwrap_err().contains("vertex 999"));
}

fn on_screen(scene: &Scene, w: u32, h: u32, p: Vec3) -> (u32, u32) {
    let c = scene.camera.view_proj(w as f32 / h as f32) * p.extend(1.0);
    let (x, y) = (c.x / c.w, c.y / c.w);
    (((x * 0.5 + 0.5) * w as f32) as u32, ((0.5 - y * 0.5) * h as f32) as u32)
}

#[test]
fn sky_shadows_textures_and_reflections() {
    let (w, h) = (320, 240);
    let Ok(mut r) = Renderer::headless(w, h) else {
        assert!(std::env::var_os("HEARTH_NEED_GPU").is_none(), "no graphics");
        return;
    };
    // A checker floor texture.
    let n = 64;
    let checker: Vec<u8> = (0..n * n).flat_map(|i| if ((i % n) / 8 + (i / n) / 8) % 2 == 0 { [200, 200, 200, 255] } else { [60, 60, 60, 255] }).collect();
    let tex = r.add_texture(&checker, n, n).unwrap();
    assert!(r.add_texture(&checker[..10], n, n).is_err());
    let mut floor = shapes::plane(12.0);
    for v in &mut floor.vertices {
        v.uv = [v.uv[0] * 6.0, v.uv[1] * 6.0];
    }
    let floor = r.add_mesh(&floor).unwrap();
    let ball = r.add_mesh(&shapes::sphere(32, 48)).unwrap();
    let block = r.add_mesh(&shapes::cube()).unwrap();
    let mut scene = Scene {
        camera: Camera { eye: Vec3::new(0.0, -6.0, 2.5), target: Vec3::new(0.0, 0.0, 0.5), ..Camera::default() },
        sun: Some(Sun { direction: Vec3::new(0.5, -0.8, -1.0), color: [1.0, 0.95, 0.85], intensity: 3.0 }),
        sky: Some(Sky::default()),
        ..Scene::default()
    };
    scene.objects.push(Object { mesh: floor, transform: Mat4::IDENTITY, material: Material { color: [1.0; 4], roughness: 0.9, texture: Some(tex), ..Material::default() } });
    scene.objects.push(Object { mesh: ball, transform: Mat4::from_translation(Vec3::new(0.0, 0.0, 1.0)), material: Material { color: [0.8, 0.1, 0.1, 1.0], roughness: 0.35, ..Material::default() } });
    let block_at = Vec3::new(-2.2, 0.3, 0.5);
    scene.objects.push(Object {
        mesh: block,
        transform: Mat4::from_translation(block_at) * Mat4::from_rotation_z(0.6),
        material: Material { color: [1.0, 0.78, 0.34, 1.0], metallic: 1.0, roughness: 0.3, ..Material::default() },
    });
    let lit = r.render(&scene).unwrap();
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../target/renders");
    save_png(&dir.join("sky-shadows-textures.png"), &lit, w, h).unwrap();
    scene.shadows = false;
    let flat = r.render(&scene).unwrap();
    save_png(&dir.join("no-shadows.png"), &flat, w, h).unwrap();

    let sky = pixel(&lit, w, w / 2, 3);
    assert!(sky[2] > sky[0], "the sky at the top is blue: {sky:?}");
    // The ball's shadow falls toward the camera, on the floor.
    let (sx, sy) = on_screen(&scene, w, h, Vec3::new(0.5, -0.8, 0.0));
    let (a, b) = (bright(pixel(&lit, w, sx, sy)), bright(pixel(&flat, w, sx, sy)));
    assert!(a * 10 < b * 7, "the ball's shadow should darken the floor at {sx},{sy}: {a} with shadows, {b} without");
    // Gold reflects the sky, so it looks gold, not brown.
    let (gx, gy) = on_screen(&scene, w, h, block_at + Vec3::new(0.0, -0.5, 0.2));
    let g = pixel(&lit, w, gx, gy);
    assert!(g[0] > 90 && g[0] > g[2] + 30, "the gold block should look gold at {gx},{gy}: {g:?}");
    // The checker shows: light and dark squares on the floor.
    let (fx, fy) = on_screen(&scene, w, h, Vec3::new(-1.5, -2.5, 0.0));
    let row: Vec<u32> = (0..40).map(|d| bright(pixel(&flat, w, (fx + d).min(w - 1), fy))).collect();
    let (lo, hi) = (row.iter().min().unwrap(), row.iter().max().unwrap());
    assert!(hi - lo > 60, "the floor texture should show (brightness {lo} to {hi})");
}

#[test]
fn detail_models_draw_full_up_close_and_fewer_triangles_far_away() {
    let (w, h) = (256, 192);
    let Ok(mut r) = Renderer::headless(w, h) else {
        assert!(std::env::var_os("HEARTH_NEED_GPU").is_none(), "no graphics");
        return;
    };
    // A bumpy ball of about 20 000 triangles, built into detail levels.
    let ball = shapes::sphere(100, 100);
    let mesh = hearth_detail::Mesh {
        vertices: ball.vertices.iter().map(|v| {
            let bump = 1.0 + 0.04 * (v.pos[0] * 23.0).sin() * (v.pos[1] * 19.0).cos();
            hearth_detail::Vertex { pos: [v.pos[0] * bump, v.pos[1] * bump, v.pos[2] * bump], normal: v.normal, uv: v.uv }
        }).collect(),
        indices: ball.indices.clone(),
    };
    let detail = hearth_detail::build(&mesh).unwrap();
    let id = r.add_detail(detail).unwrap();
    let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../target/renders");
    let mut scene = Scene {
        sun: Some(Sun { direction: Vec3::new(0.3, 0.6, -1.0), color: [1.0, 1.0, 1.0], intensity: 3.0 }),
        sky: Some(Sky::default()),
        ..Scene::default()
    };
    scene.details.push(hearth_render::DetailObject { detail: id, transform: Mat4::from_translation(Vec3::new(0.0, 0.0, 1.0)), material: Material { color: [0.7, 0.6, 0.5, 1.0], ..Material::default() }, max_pixels: 1.0 });
    let mut counts = Vec::new();
    for (name, dist) in [("detail-near", 2.5f32), ("detail-mid", 12.0), ("detail-far", 60.0)] {
        scene.camera = Camera { eye: Vec3::new(0.0, -dist, 1.0 + dist * 0.2), target: Vec3::new(0.0, 0.0, 1.0), ..Camera::default() };
        let img = r.render(&scene).unwrap();
        save_png(&dir.join(format!("{name}.png")), &img, w, h).unwrap();
        counts.push(r.last_triangles);
    }
    assert!(counts[0] > counts[1] && counts[1] > counts[2], "farther must mean fewer triangles: {counts:?}");
    // The ball's uv seam and poles stay (so textures never tear), which limits how simple it gets.
    assert!(counts[2] * 5 < counts[0], "far away should be a small part of the full model: {counts:?}");
}
