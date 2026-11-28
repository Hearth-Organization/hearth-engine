use hearth_anim::prefab::{changes, instance, push_to_original};
use hearth_anim::states::{Machine, Rule, State, Test, Values};
use hearth_anim::timeline::{Ease, Key, Mark, Repeat, Timeline, Track};
use serde_json::json;

fn machine() -> Machine {
    let s = |n: &str, len: f32, looping: bool| State { name: n.into(), clip: format!("{n}.anim"), length: len, looping };
    let r = |from: &str, to: &str, when: &[&str], blend: f32, at_end: bool| Rule { from: from.into(), to: to.into(), when: when.iter().map(|t| Test::parse(t).unwrap()).collect(), blend, at_end };
    Machine::new(
        vec![s("idle", 2.0, true), s("walk", 1.0, true), s("run", 0.8, true), s("attack", 0.6, false)],
        vec![
            r("*", "attack", &["attacking"], 0.1, false),
            r("attack", "idle", &[], 0.2, true),
            r("idle", "walk", &["speed > 0.1"], 0.25, false),
            r("walk", "run", &["speed > 4"], 0.2, false),
            r("run", "walk", &["speed < 4"], 0.2, false),
            r("walk", "idle", &["speed < 0.1"], 0.25, false),
        ],
    )
    .unwrap()
}

#[test]
fn states_follow_the_rules_and_blend() {
    let mut m = machine();
    let mut v = Values::default();
    assert_eq!(m.step(0.1, &v).clip, "idle.anim");
    v.set("speed", 2.0);
    let p = m.step(0.1, &v);
    assert_eq!(m.state(), "walk");
    assert_eq!(p.from.as_ref().map(|f| f.0.as_str()), Some("idle.anim"), "blending from idle");
    assert!(p.weight < 0.01);
    let p = m.step(0.125, &v);
    assert!((p.weight - 0.5).abs() < 0.01, "halfway through the blend: {}", p.weight);
    let p = m.step(0.2, &v);
    assert!(p.from.is_none() && p.weight == 1.0, "blend over");
    v.set("speed", 6.0);
    m.step(0.1, &v);
    assert_eq!(m.state(), "run");
    // Attacking from anywhere; the attack plays to its end, then back to idle.
    v.flag("attacking", true);
    m.step(0.1, &v);
    assert_eq!(m.state(), "attack");
    v.flag("attacking", false);
    m.step(0.3, &v);
    assert_eq!(m.state(), "attack", "not finished yet");
    m.step(0.4, &v);
    assert_eq!(m.state(), "idle");
}

#[test]
fn bad_machines_and_rules_are_explained() {
    assert!(Machine::new(vec![], vec![]).is_err());
    let s = State { name: "idle".into(), clip: "i".into(), length: 1.0, looping: true };
    let r = Rule { from: "idle".into(), to: "fly".into(), when: vec![], blend: 0.0, at_end: false };
    assert!(Machine::new(vec![s], vec![r]).unwrap_err().contains("\"fly\""));
    assert!(Test::parse("speed > fast").unwrap_err().contains("isn't a number"));
    assert_eq!(Test::parse("not grounded").unwrap(), Test::Off("grounded".into()));
}

#[test]
fn the_timeline_eases_loops_and_fires_marks() {
    let door = Track::new("door.angle", vec![Key { time: 0.0, value: 0.0, ease: Ease::Smooth }, Key { time: 1.0, value: 90.0, ease: Ease::Linear }]);
    assert_eq!(door.value(-1.0), Some(0.0));
    assert_eq!(door.value(0.5), Some(45.0), "smooth is half way at half time");
    assert!(door.value(0.25).unwrap() < 22.5, "smooth starts gently");
    assert_eq!(door.value(2.0), Some(90.0));
    let step = Track::new("lamp.on", vec![Key { time: 0.0, value: 0.0, ease: Ease::Step }, Key { time: 1.0, value: 1.0, ease: Ease::Step }]);
    assert_eq!(step.value(0.99), Some(0.0));

    let mut t = Timeline::new(vec![door.clone()], vec![Mark { time: 0.2, event: "creak".into() }, Mark { time: 1.0, event: "thud".into() }], Repeat::Once);
    let (_, m) = t.step(0.5);
    assert_eq!(m, ["creak"]);
    let (vals, m) = t.step(1.0);
    assert_eq!(m, ["thud"]);
    assert_eq!(vals, [("door.angle".to_string(), 90.0)]);
    assert!(t.finished());

    let mut l = Timeline::new(vec![door], vec![Mark { time: 0.2, event: "creak".into() }], Repeat::Loop);
    let (_, m) = l.step(2.5);
    assert_eq!(m.len(), 3, "a long step still fires every loop's mark: {m:?}");
    let mut p = Timeline::new(vec![Track::new("x", vec![Key { time: 0.0, value: 0.0, ease: Ease::Linear }, Key { time: 1.0, value: 10.0, ease: Ease::Linear }])], vec![], Repeat::PingPong);
    let (v, _) = p.step(1.5);
    assert_eq!(v[0].1, 5.0, "on the way back");
}

#[test]
fn linked_prefabs_follow_the_original_and_keep_their_own_changes() {
    let mut door = json!({"model": "door.glb", "locked": false, "light": {"color": "white", "power": 5}, "sound": "creak"});
    let edited = json!({"model": "door.glb", "locked": true, "light": {"color": "red", "power": 5}});
    let mut mine = changes(&door, &edited);
    assert_eq!(mine, json!({"locked": true, "light": {"color": "red"}, "sound": null}), "only what really differs");
    assert_eq!(instance(&door, &mine), edited);

    // The original changes: the copy follows, except where it has its own value.
    door["light"]["power"] = json!(8);
    door["locked"] = json!(false);
    door["model"] = json!("door2.glb");
    let now = instance(&door, &mine);
    assert_eq!(now, json!({"model": "door2.glb", "locked": true, "light": {"color": "red", "power": 8}}));

    // Apply one change to the original.
    push_to_original(&mut door, &mut mine, &["light", "color"]).unwrap();
    assert_eq!(door["light"]["color"], "red");
    assert_eq!(mine, json!({"locked": true, "light": {}, "sound": null}));
    assert!(push_to_original(&mut door, &mut mine, &["nothing"]).is_err());
}
