use glam::Vec3;
use hearth_audio::acoustics::{walls_between, Settings};
use hearth_audio::{Audio, Emitter, Listener};
use hearth_physics::{Body, Physics, Shape, ShapeKind};
use hearth_world::{Transform, World};
use kira::backend::mock::{MockBackend, MockBackendSettings};
use kira::AudioManagerSettings;

fn wall(w: &mut World, x: f32) {
    let e = w.spawn_named("Wall", Transform::at(Vec3::new(x, 0.0, 1.0)));
    w.insert(e, Body::Fixed);
    w.insert(e, Shape::new(ShapeKind::Box { half: Vec3::new(0.1, 10.0, 3.0) }));
}

#[test]
fn walls_are_counted_and_muffle_more_each() {
    let mut w = World::new();
    wall(&mut w, 3.0);
    wall(&mut w, 6.0);
    let mut p = Physics::new();
    p.step(&mut w, 1.0 / 60.0);
    let ear = Vec3::new(0.0, 0.0, 1.0);
    assert_eq!(walls_between(&p, ear, Vec3::new(2.0, 0.0, 1.0), None, None), 0);
    assert_eq!(walls_between(&p, ear, Vec3::new(4.0, 0.0, 1.0), None, None), 1);
    assert_eq!(walls_between(&p, ear, Vec3::new(9.0, 0.0, 1.0), None, None), 2);
    let s = Settings::default();
    let (c0, d0) = s.muffle(0);
    let (c1, d1) = s.muffle(1);
    let (c2, d2) = s.muffle(2);
    assert_eq!((c0, d0), (20_000.0, 0.0));
    assert!(c1 < c0 && c2 < c1 && d2 < d1 && d1 < 0.0);
    assert_eq!(s.muffle(9), s.muffle(4), "it stops getting quieter after a few walls");
}

#[test]
fn halls_echo_more_than_small_rooms_and_outdoors_not_at_all() {
    let s = Settings::default();
    let outdoors = s.room(|_| None);
    assert_eq!(outdoors.feedback, 0.0);
    let small = s.room(|_| Some(2.0));
    let hall = s.room(|_| Some(25.0));
    assert!(hall.feedback > small.feedback && hall.send_db > small.send_db, "{small:?} {hall:?}");
    let half_open = s.room(|d| (d.x > 0.0).then_some(25.0));
    assert!(half_open.feedback < hall.feedback);
}

#[test]
fn emitters_play_follow_their_entities_and_stop() {
    let Ok(mut audio) = Audio::<MockBackend>::new(AudioManagerSettings { backend_settings: MockBackendSettings::default(), ..Default::default() }) else {
        panic!("the test sound device wouldn't start");
    };
    audio.add_frames("hum", 48_000, vec![kira::Frame { left: 0.1, right: 0.1 }; 4800]);
    assert!(audio.has("hum"));
    let mut w = World::new();
    wall(&mut w, 3.0);
    let ear = w.spawn_named("Ear", Transform::at(Vec3::new(0.0, 0.0, 1.0)));
    w.insert(ear, Listener);
    let hum = w.spawn_named("Hum", Transform::at(Vec3::new(5.0, 0.0, 1.0)));
    w.insert(hum, Emitter { sound: "hum".into(), volume: 0.0, looping: true, range: 30.0 });
    let lost = w.spawn_named("Lost", Transform::at(Vec3::ZERO));
    w.insert(lost, Emitter { sound: "nothing".into(), volume: 0.0, looping: false, range: 10.0 });
    let mut p = Physics::new();
    p.step(&mut w, 1.0 / 60.0);
    w.update_transforms();
    let problems = audio.update(&w, Some(&p), 1.0 / 60.0);
    assert!(problems.iter().any(|m| m.contains("no sound called \"nothing\"")), "{problems:?}");
    assert_eq!(audio.playing(), 1);
    audio.update(&w, Some(&p), 1.0 / 60.0);
    assert_eq!(audio.walls(hum), Some(1), "the hum is behind the wall");
    w.get_mut::<Transform>(hum).unwrap().translation.x = 2.0;
    w.update_transforms();
    audio.update(&w, Some(&p), 1.0 / 60.0);
    assert_eq!(audio.walls(hum), Some(0), "moved in front of the wall");
    w.despawn(hum);
    audio.update(&w, Some(&p), 1.0 / 60.0);
    assert_eq!(audio.playing(), 0);
}
