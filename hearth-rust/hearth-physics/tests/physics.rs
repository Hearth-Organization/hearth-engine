use glam::Vec3;
use hearth_physics::{Body, Physics, Shape, ShapeKind, Touch};
use hearth_world::{Transform, World};

fn floor(w: &mut World) -> hearth_world::Entity {
    let f = w.spawn_named("Floor", Transform::at(Vec3::new(0.0, 0.0, -0.5)));
    w.insert(f, Body::Fixed);
    w.insert(f, Shape::new(ShapeKind::Box { half: Vec3::new(20.0, 20.0, 0.5) }));
    f
}

#[test]
fn a_ball_falls_lands_and_the_touch_is_reported() {
    let mut w = World::new();
    let f = floor(&mut w);
    let ball = w.spawn_named("Ball", Transform::at(Vec3::new(0.0, 0.0, 5.0)));
    w.insert(ball, Body::Dynamic);
    w.insert(ball, Shape::new(ShapeKind::Ball { radius: 0.5 }));
    let mut p = Physics::new();
    let mut touches = Vec::new();
    for _ in 0..180 {
        assert!(p.step(&mut w, 1.0 / 60.0).is_empty());
        touches.extend(p.take_touches());
    }
    let z = w.get::<Transform>(ball).unwrap().translation.z;
    assert!((z - 0.5).abs() < 0.05, "the ball rests on the floor: z = {z}");
    assert!(touches.contains(&Touch::Started(f.min(ball), f.max(ball))), "{touches:?}");
    assert!(p.velocity(ball).unwrap().length() < 0.1);
}

#[test]
fn a_stack_of_boxes_stays_up_and_a_push_knocks_one_off() {
    let mut w = World::new();
    floor(&mut w);
    let mut boxes = Vec::new();
    for k in 0..4 {
        let b = w.spawn_named("Box", Transform::at(Vec3::new(0.0, 0.0, 0.5 + k as f32 * 1.0)));
        w.insert(b, Body::Dynamic);
        w.insert(b, Shape::new(ShapeKind::Box { half: Vec3::splat(0.5) }));
        boxes.push(b);
    }
    let mut p = Physics::new();
    for _ in 0..120 {
        p.step(&mut w, 1.0 / 60.0);
    }
    for (k, b) in boxes.iter().enumerate() {
        let t = w.get::<Transform>(*b).unwrap().translation;
        assert!((t.z - (0.5 + k as f32)).abs() < 0.1 && t.x.abs() < 0.1, "box {k} should still be stacked: {t}");
    }
    p.push(boxes[3], Vec3::new(8.0, 0.0, 0.0));
    for _ in 0..120 {
        p.step(&mut w, 1.0 / 60.0);
    }
    assert!(w.get::<Transform>(boxes[3]).unwrap().translation.x > 1.0, "the pushed box went off");
}

#[test]
fn rays_moved_bodies_and_problems() {
    let mut w = World::new();
    let f = floor(&mut w);
    let wall = w.spawn_named("Wall", Transform::at(Vec3::new(5.0, 0.0, 1.0)));
    w.insert(wall, Body::Fixed);
    w.insert(wall, Shape::new(ShapeKind::Box { half: Vec3::new(0.1, 3.0, 1.0) }));
    let mut p = Physics::new();
    p.step(&mut w, 1.0 / 60.0);
    let (hit, d) = p.ray(Vec3::new(0.0, 0.0, 1.0), Vec3::X, 100.0, None).unwrap();
    assert_eq!(hit, wall);
    assert!((d - 4.9).abs() < 0.01, "{d}");
    assert_eq!(p.ray(Vec3::new(0.0, 0.0, 1.0), Vec3::NEG_Z, 100.0, None).map(|h| h.0), Some(f));
    assert!(p.ray(Vec3::new(0.0, 0.0, 1.0), Vec3::NEG_X, 100.0, None).is_none());

    // A door the game moves: physics follows its transform.
    w.insert(wall, Body::Moved);
    let mut p2 = Physics::new();
    p2.step(&mut w, 1.0 / 60.0);
    w.get_mut::<Transform>(wall).unwrap().translation.x = 8.0;
    for _ in 0..3 {
        p2.step(&mut w, 1.0 / 60.0);
    }
    let (_, d) = p2.ray(Vec3::new(0.0, 0.0, 1.0), Vec3::X, 100.0, None).unwrap();
    assert!((d - 7.9).abs() < 0.05, "the moved wall is where the game put it: {d}");

    // Things physics can't do are explained.
    let child = w.spawn_named("Child", Transform::default());
    w.insert(child, Body::Dynamic);
    w.insert(child, Shape::new(ShapeKind::Ball { radius: 0.2 }));
    w.set_parent(child, Some(f)).unwrap();
    let bad = w.spawn_named("BadMesh", Transform::default());
    w.insert(bad, Body::Dynamic);
    w.insert(bad, Shape::new(ShapeKind::Mesh { vertices: vec![Vec3::ZERO, Vec3::X, Vec3::Y], triangles: vec![[0, 1, 2]] }));
    let problems = p2.step(&mut w, 1.0 / 60.0);
    assert!(problems.iter().any(|m| m.contains("can't have a parent")), "{problems:?}");
    assert!(problems.iter().any(|m| m.contains("fixed body")), "{problems:?}");
}
