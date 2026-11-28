use std::cell::RefCell;
use std::rc::Rc;

use glam::{Quat, Vec3};
use hearth_world::scene::Registry;
use hearth_world::*;
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
struct Health(f32);

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
struct Door {
    locked: bool,
}

#[test]
fn entities_components_and_ids_that_never_lie() {
    let mut w = World::new();
    let a = w.spawn_named("Player", Transform::default());
    let b = w.spawn();
    w.insert(a, Health(100.0));
    w.insert(b, Health(20.0));
    w.insert(b, Door { locked: true });
    assert_eq!(w.get::<Health>(a), Some(&Health(100.0)));
    assert_eq!(w.count::<Health>(), 2);
    assert_eq!(w.with_both::<Health, Door>(), vec![b]);
    w.for_each_mut::<Health>(|_, h| h.0 -= 5.0);
    assert_eq!(w.get::<Health>(b), Some(&Health(15.0)));
    assert_eq!(w.find("Player"), Some(a));
    w.despawn(b);
    assert!(!w.is_alive(b));
    assert_eq!(w.get::<Health>(b), None);
    let c = w.spawn();
    assert_eq!(c.index, b.index, "the slot is used again");
    assert!(!w.is_alive(b) && w.is_alive(c), "but the old id doesn't point at the new entity");
    w.insert(b, Health(1.0));
    assert_eq!(w.count::<Health>(), 1, "giving a removed entity a component does nothing");
    assert_eq!(w.remove::<Health>(a), Some(Health(95.0)));
    assert!(!w.has::<Health>(a));
}

#[test]
fn children_move_with_their_parents_and_go_with_them() {
    let mut w = World::new();
    let car = w.spawn_named("Car", Transform::at(Vec3::new(10.0, 0.0, 0.0)));
    let wheel = w.spawn_named("Wheel", Transform::at(Vec3::new(1.0, 0.0, 0.0)));
    let bolt = w.spawn_named("Bolt", Transform::at(Vec3::new(0.0, 0.5, 0.0)));
    w.set_parent(wheel, Some(car)).unwrap();
    w.set_parent(bolt, Some(wheel)).unwrap();
    assert!(w.set_parent(car, Some(bolt)).unwrap_err().contains("its own parent"));
    w.get_mut::<Transform>(car).unwrap().rotation = Quat::from_rotation_z(std::f32::consts::FRAC_PI_2);
    w.update_transforms();
    let at = w.get::<GlobalTransform>(bolt).unwrap().0.transform_point3(Vec3::ZERO);
    assert!((at - Vec3::new(9.5, 1.0, 0.0)).length() < 1e-4, "the bolt follows the turned car: {at}");
    w.despawn(car);
    assert!(!w.is_alive(wheel) && !w.is_alive(bolt), "children go with their parent");
    assert!(w.entities().is_empty());
}

#[test]
fn systems_run_in_order_each_frame_with_time() {
    let mut w = World::new();
    let log = Rc::new(RefCell::new(Vec::new()));
    let mut s = Schedule::default();
    let (l1, l2) = (log.clone(), log.clone());
    s.add("move", move |w| {
        let dt = w.resource::<Time>().unwrap().delta;
        w.for_each_mut::<Transform>(|_, t| t.translation.x += 2.0 * dt);
        l1.borrow_mut().push("move");
    })
    .add("report", move |_| l2.borrow_mut().push("report"));
    let e = w.spawn_named("Ball", Transform::default());
    for _ in 0..10 {
        s.run(&mut w, 0.1);
    }
    assert_eq!(s.names(), ["move", "report"]);
    assert_eq!(log.borrow()[..2], ["move", "report"]);
    assert_eq!(w.resource::<Time>().unwrap().frame, 10);
    assert!((w.get::<GlobalTransform>(e).unwrap().0.w_axis.x - 2.0).abs() < 1e-4);
}

#[test]
fn levels_save_and_load_with_their_parents_and_components() {
    let mut w = World::new();
    let room = w.spawn_named("Room", Transform::at(Vec3::new(5.0, 0.0, 0.0)));
    let door = w.spawn_named("Door", Transform::at(Vec3::new(0.0, 2.0, 0.0)));
    w.insert(door, Door { locked: true });
    w.set_parent(door, Some(room)).unwrap();
    let mut reg = Registry::new();
    reg.add::<Door>("Door");
    let dir = std::env::temp_dir().join(format!("hearth-world-{}", std::process::id()));
    let path = dir.join("level.json");
    reg.save_file(&w, &path).unwrap();

    let mut back = World::new();
    let (ids, notes) = reg.load_file(&mut back, &path).unwrap();
    assert!(notes.is_empty());
    assert_eq!(ids.len(), 2);
    let d = back.find("Door").unwrap();
    assert_eq!(back.get::<Door>(d), Some(&Door { locked: true }));
    assert_eq!(back.get::<Parent>(d).map(|p| p.0), back.find("Room"));
    let at = back.get::<GlobalTransform>(d).unwrap().0.transform_point3(Vec3::ZERO);
    assert!((at - Vec3::new(5.0, 2.0, 0.0)).length() < 1e-5);

    // A game without the Door component still loads the level, and says what it left out.
    let plain = Registry::new();
    let mut other = World::new();
    let (_, notes) = plain.load_file(&mut other, &path).unwrap();
    assert!(notes[0].contains("\"Door\" isn't a component this game has"));
    assert!(plain.load(&mut World::new(), &serde_json::json!({"format": 9})).unwrap_err().contains("isn't a Hearth level"));
    let _ = std::fs::remove_dir_all(dir);
}
