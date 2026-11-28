use glam::{Mat4, Vec3};
use hearth_render::{save_png, shapes, Camera, Material, Object, PointLight, Renderer, Scene, Sun};

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
    assert!(bright(sky) < 60, "the corner should be the background: {sky:?}");
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
