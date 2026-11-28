use std::collections::BTreeSet;

use hearth_kits::dialogue::{Dialogue, Say, Talk};
use hearth_kits::inventory::{can_craft, craft, Inventory, ItemKind, Recipe};
use hearth_kits::platformer::{Character, Input, Tiles};
use hearth_kits::quests::{Journal, Note, Quest, State};
use hearth_kits::tactics::{Battle, Fighter};
use hearth_kits::towers::{Defense, EnemyKind, Happened, TowerKind, Wave};
use hearth_rts::{Cell, Grid};

const LEVEL: &str = "
..........................
..........................
..........................
..........##########......
..........................
..........................
..........................
##########......##########
##########......##########
";

fn steps(c: &mut Character, t: &Tiles, input: Input, seconds: f32) -> Vec<hearth_kits::platformer::Happened> {
    (0..(seconds * 60.0) as i32).map(|_| c.step(t, input, 1.0 / 60.0)).collect()
}

#[test]
fn platformer_lands_runs_and_jumps_the_right_height() {
    let t = Tiles::from_text(LEVEL, 1.0);
    let mut c = Character::new(3.0, 4.0);
    let h = steps(&mut c, &t, Input::default(), 1.5);
    assert!(c.on_ground && (c.y - 2.0).abs() < 1e-3, "lands on the floor at 2: {}", c.y);
    assert_eq!(h.iter().filter(|h| h.landed).count(), 1);

    let start = c.x;
    steps(&mut c, &t, Input { run: 1.0, jump_held: false }, 0.5);
    assert!((c.vx - c.feel.run_speed).abs() < 1e-3, "reaches full speed");
    assert!(c.x > start + 2.0);
    // Stop before the gap.
    steps(&mut c, &t, Input::default(), 0.5);
    assert!(c.vx.abs() < 1e-3);

    // A full jump goes about jump_height.
    let floor = c.y;
    let mut top = floor;
    let mut jumped = false;
    for _ in 0..90 {
        let h = c.step(&t, Input { run: 0.0, jump_held: true }, 1.0 / 60.0);
        jumped |= h.jumped;
        top = top.max(c.y);
    }
    assert!(jumped);
    assert!((top - floor - c.feel.jump_height).abs() < 0.2, "jumped {}", top - floor);

    // A tap makes a short hop.
    steps(&mut c, &t, Input::default(), 1.0);
    let mut top2 = c.y;
    c.step(&t, Input { run: 0.0, jump_held: true }, 1.0 / 60.0);
    for _ in 0..60 {
        c.step(&t, Input::default(), 1.0 / 60.0);
        top2 = top2.max(c.y);
    }
    assert!(top2 - floor < c.feel.jump_height * 0.5, "short hop {}", top2 - floor);
}

#[test]
fn platformer_coyote_time_and_jump_buffer() {
    let t = Tiles::from_text(LEVEL, 1.0);
    // Run off the ledge at x=10 and press jump just after leaving it: still jumps.
    let mut c = Character::new(8.0, 2.0);
    steps(&mut c, &t, Input::default(), 0.2);
    let mut left_ground_at = None;
    let mut jumped = false;
    for i in 0..120 {
        let held = left_ground_at.map(|s: i32| i >= s + 3).unwrap_or(false);
        let h = c.step(&t, Input { run: 1.0, jump_held: held }, 1.0 / 60.0);
        if !c.on_ground && left_ground_at.is_none() && !h.jumped {
            left_ground_at = Some(i);
        }
        if h.jumped {
            jumped = true;
            break;
        }
    }
    assert!(jumped, "a jump pressed 3 steps after the ledge still works");

    // Pressing jump just before landing jumps on landing.
    let mut c = Character::new(3.0, 3.0);
    let mut jumped_on_landing = false;
    let mut pressed = false;
    for _ in 0..120 {
        let near = !c.on_ground && c.y < 2.3 && c.vy < 0.0;
        if near {
            pressed = true;
        }
        let h = c.step(&t, Input { run: 0.0, jump_held: pressed }, 1.0 / 60.0);
        if h.jumped {
            jumped_on_landing = true;
            break;
        }
    }
    assert!(jumped_on_landing);
}

#[test]
fn platformer_walls_and_ceilings_stop_it() {
    let t = Tiles::from_text(LEVEL, 1.0);
    let mut c = Character::new(17.0, 2.0);
    steps(&mut c, &t, Input::default(), 0.2);
    let mut bumped = false;
    for _ in 0..60 {
        bumped |= c.step(&t, Input { run: 0.0, jump_held: true }, 1.0 / 60.0).bumped_head;
    }
    assert!(bumped, "the platform at height 5 is overhead");
    assert!(c.y + c.height <= 5.0 + 1e-3);
}

const TALK: &str = "
== gate
Guard: Halt! Who goes there?
* A friend. -> friend
* (if has_pass) Show the pass. -> pass
* (if not has_pass) Run. -> end

== friend
Guard: Then pass, friend. [set guard_likes_you]
-> end

== pass
Guard: Very well.
-> end
";

#[test]
fn dialogue_runs_with_choices_and_remembers() {
    let d = Dialogue::parse(TALK).unwrap();
    let mut flags = BTreeSet::new();
    let mut t = Talk::start(&d, None);
    assert_eq!(t.next(&mut flags), Say::Line { who: "Guard".into(), text: "Halt! Who goes there?".into() });
    assert_eq!(t.next(&mut flags), Say::Choose(vec!["A friend.".into(), "Run.".into()]));
    t.choose(0, &flags).unwrap();
    assert_eq!(t.next(&mut flags), Say::Line { who: "Guard".into(), text: "Then pass, friend.".into() });
    assert!(flags.contains("guard_likes_you"));
    assert_eq!(t.next(&mut flags), Say::End);

    flags.insert("has_pass".into());
    let mut t = Talk::start(&d, Some("gate"));
    t.next(&mut flags);
    assert_eq!(t.next(&mut flags), Say::Choose(vec!["A friend.".into(), "Show the pass.".into()]));
    assert!(t.choose(5, &flags).is_err());
}

#[test]
fn dialogue_mistakes_are_explained() {
    assert!(Dialogue::parse("Guard: hi").unwrap_err().contains("start with a part"));
    assert!(Dialogue::parse("== a\n* go -> nowhere").unwrap_err().contains("nowhere"));
    assert!(Dialogue::parse("== a\n* go somewhere").unwrap_err().contains("line 2"));
}

#[test]
fn quests_count_events_and_unlock_the_next() {
    let mut j = Journal::default();
    j.add(Quest::new("wood", "Firewood").objective("Collect wood", "picked_up:wood", 5).reward("gold", 10)).unwrap();
    j.add(Quest::new("smith", "The smith").after("wood").in_order().objective("Talk to the smith", "talked:smith", 1).objective("Bring iron", "picked_up:iron", 2)).unwrap();
    assert!(j.add(Quest::new("wood", "again")).is_err());
    assert_eq!(j.refresh(), vec![Note::Opened("wood".into())]);
    assert!(j.start("smith").is_err());
    j.start("wood").unwrap();
    j.happened("picked_up:wood", 3);
    let notes = j.happened("picked_up:wood", 4);
    assert!(notes.contains(&Note::Completed { quest: "wood".into(), rewards: vec![("gold".into(), 10)] }));
    assert!(notes.contains(&Note::Opened("smith".into())));
    j.start("smith").unwrap();
    j.happened("picked_up:iron", 2);
    assert_eq!(j.get("smith").unwrap().objectives[1].done, 0, "in order: iron doesn't count before talking");
    j.happened("talked:smith", 1);
    j.happened("picked_up:iron", 2);
    assert_eq!(j.get("smith").unwrap().state, State::Complete);
}

#[test]
fn inventory_stacks_and_crafting() {
    let kinds = vec![ItemKind { name: "wood".into(), max_stack: 10 }, ItemKind { name: "plank".into(), max_stack: 20 }, ItemKind { name: "sword".into(), max_stack: 1 }];
    let mut inv = Inventory::new(3);
    assert_eq!(inv.add(&kinds[0], 25), 0);
    assert_eq!(inv.count("wood"), 25);
    assert_eq!(inv.add(&kinds[0], 10), 5, "three slots of ten: five don't fit");
    assert!(inv.take("wood", 40).is_err());
    assert_eq!(inv.count("wood"), 30, "a failed take takes nothing");

    let planks = Recipe { name: "Planks".into(), needs: vec![("wood".into(), 10)], makes: vec![("plank".into(), 4)], at: Some("Workbench".into()) };
    assert!(can_craft(&inv, &planks, &kinds, &[]).unwrap_err().contains("Workbench"));
    let bench = vec!["Workbench".to_string()];
    craft(&mut inv, &planks, &kinds, &bench).unwrap();
    assert_eq!(inv.count("wood"), 20);
    assert_eq!(inv.count("plank"), 4);

    let sword = Recipe { name: "Sword".into(), needs: vec![("plank".into(), 9)], makes: vec![("sword".into(), 1)], at: None };
    assert!(can_craft(&inv, &sword, &kinds, &[]).unwrap_err().contains("5 plank"));
    // Slots: wood, wood, plank. Joining two stacks of wood frees a slot.
    inv.take("wood", 15).unwrap();
    inv.move_slot(1, 0, &kinds);
    assert_eq!(inv.slots.iter().filter(|s| s.is_none()).count(), 1);
}

#[test]
fn tactics_turns_moves_and_attacks() {
    let grid = Grid::from_text("......\n..#...\n......\n", 1.0);
    let knight = Fighter { name: "Knight".into(), team: 0, at: Cell { x: 0, y: 1 }, health: 10, moves: 3, speed: 5, range: 1, damage: 6 };
    let archer = Fighter { name: "Archer".into(), team: 1, at: Cell { x: 5, y: 1 }, health: 8, moves: 2, speed: 7, range: 3, damage: 3 };
    let mut b = Battle::new(grid, vec![knight, archer]);
    assert_eq!(b.current(), Some(1), "the faster one goes first");
    assert!(b.attack(0).is_err(), "out of range");
    b.walk(Cell { x: 3, y: 1 }).unwrap();
    assert!(b.walk(Cell { x: 3, y: 0 }).is_err(), "one move a turn");
    assert_eq!(b.attack(0).unwrap(), 3);
    assert_eq!(b.end_turn(), None);
    assert_eq!(b.current(), Some(0));
    let r = b.reachable(0);
    assert!(!r.contains_key(&Cell { x: 2, y: 1 }), "a wall");
    assert!(!r.contains_key(&Cell { x: 3, y: 1 }), "someone stands there");
    assert!(r.contains_key(&Cell { x: 2, y: 0 }));
    b.walk(Cell { x: 2, y: 2 }).unwrap();
    assert!(b.attack(1).is_err(), "not next to it yet");
    b.end_turn();
    b.end_turn();
    assert_eq!(b.round, 2);
    b.walk(Cell { x: 3, y: 2 }).unwrap();
    b.attack(1).unwrap();
    b.end_turn();
    b.end_turn();
    b.attack(1).unwrap();
    assert_eq!(b.end_turn(), Some(0), "the knight's team wins");
}

#[test]
fn tower_defense_waves_paths_and_blocking() {
    let grid = Grid::new(12, 5, 1.0);
    let enemies = vec![EnemyKind { name: "Slime".into(), health: 10.0, speed: 2.0, gold: 5 }];
    let towers = vec![TowerKind { name: "Arrow".into(), cost: 30, range: 2.5, damage: 5.0, every: 0.5 }];
    let waves = vec![Wave { enemy: 0, count: 3, gap: 1.0, delay: 0.0 }, Wave { enemy: 0, count: 2, gap: 1.0, delay: 1.0 }];
    let mut d = Defense::new(grid, Cell { x: 0, y: 2 }, Cell { x: 11, y: 2 }, enemies, towers, waves);
    for y in 0..5 {
        if y != 4 {
            d.build(0, Cell { x: 5, y }).unwrap_or_else(|e| panic!("{e}"));
            d.gold += 30;
        }
    }
    assert!(d.build(0, Cell { x: 5, y: 4 }).unwrap_err().contains("block the way"));
    assert!(d.build(0, Cell { x: 0, y: 2 }).is_err());
    let mut all = Vec::new();
    for _ in 0..(60 * 30) {
        all.extend(d.step(1.0 / 60.0));
        if d.is_over() {
            break;
        }
    }
    assert!(all.contains(&Happened::WaveStarted(0)) && all.contains(&Happened::WaveStarted(1)));
    assert!(all.iter().any(|h| matches!(h, Happened::Shot { .. })));
    let killed: u32 = all.iter().filter_map(|h| if let Happened::Killed { gold } = h { Some(*gold) } else { None }).sum();
    let through = all.iter().filter(|h| **h == Happened::GotThrough).count() as u32;
    assert_eq!(killed / 5 + through, 5, "every enemy was killed or got through");
    assert_eq!(d.lives, 20 - through);
    assert!(all.last() == Some(&Happened::Won) || all.last() == Some(&Happened::Lost));
}
