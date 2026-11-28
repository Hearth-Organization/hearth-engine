use hearth_rts::flow::{FlowCache, FlowField};
use hearth_rts::fog::{line_of_sight, Fog, Sight};
use hearth_rts::select::Selection;
use hearth_rts::{Attack, Cell, Event, Grid, Kind, Order, Pos, World};

const MAP: &str = "
....................
....................
.........#..........
.........#..........
.........#..........
.........#..........
.........#..........
.........#..........
....................
....................
";

fn kinds() -> Vec<Kind> {
    let mut worker = Kind::unit("Worker", 3.0, 0.3, 40.0);
    worker.carry = 10.0;
    worker.gather_rate = 5.0;
    worker.builds = vec!["House".into(), "Hall".into()];
    worker.cost = vec![("gold".into(), 50)];
    worker.time = 2.0;
    let mut soldier = Kind::unit("Soldier", 3.0, 0.4, 60.0);
    soldier.attack = Some(Attack { range: 0.5, damage: 10.0, every: 1.0 });
    let mut hall = Kind::building("Hall", 2, 2, 500.0);
    hall.trains = vec!["Worker".into()];
    hall.drop_off = true;
    hall.supply = 2;
    hall.cost = vec![("gold".into(), 100)];
    hall.time = 4.0;
    let mut house = Kind::building("House", 1, 1, 100.0);
    house.supply = 5;
    house.cost = vec![("gold".into(), 30)];
    house.time = 3.0;
    vec![worker, soldier, hall, house]
}

fn run(w: &mut World, seconds: f32) {
    for _ in 0..(seconds * 20.0) as i32 {
        w.step(0.05);
    }
}

#[test]
fn paths_go_around_walls_and_never_cut_corners() {
    let g = Grid::from_text(MAP, 1.0);
    let f = FlowField::new(&g, Cell { x: 15, y: 5 });
    let path = f.path(Cell { x: 3, y: 5 });
    assert_eq!(path.first(), Some(&Cell { x: 3, y: 5 }));
    assert_eq!(path.last(), Some(&Cell { x: 15, y: 5 }));
    assert!(path.iter().all(|c| g.walkable(*c)), "the path goes through a wall");
    assert!(path.iter().any(|c| c.y < 2 || c.y > 7), "it must go round the wall's ends");
    for w in path.windows(2) {
        let (a, b) = (w[0], w[1]);
        assert!((a.x - b.x).abs() <= 1 && (a.y - b.y).abs() <= 1);
        if a.x != b.x && a.y != b.y {
            assert!(g.walkable(Cell { x: b.x, y: a.y }) && g.walkable(Cell { x: a.x, y: b.y }), "cut a wall's corner at {a:?}->{b:?}");
        }
    }
}

#[test]
fn closed_off_places_are_unreachable_and_orders_into_walls_go_beside_them() {
    let g = Grid::from_text("....#...\n....#...\n....#...\n", 1.0);
    let f = FlowField::new(&g, Cell { x: 6, y: 1 });
    assert!(!f.reachable(Cell { x: 1, y: 1 }));
    assert!(f.path(Cell { x: 1, y: 1 }).is_empty());
    let into_wall = FlowField::new(&g, Cell { x: 4, y: 1 });
    assert!(g.walkable(into_wall.goal));
}

#[test]
fn many_units_share_one_path_and_mud_is_avoided() {
    let g = Grid::from_text("..........\n..,,,,,,..\n..,,,,,,..\n..........\n", 1.0);
    let mut cache = FlowCache::new(8);
    for _ in 0..500 {
        cache.get(&g, Cell { x: 9, y: 2 });
    }
    assert_eq!(cache.len(), 1, "500 units heading to one place need one field");
    let f = FlowField::new(&g, Cell { x: 9, y: 1 });
    let path = f.path(Cell { x: 0, y: 1 });
    let muddy = path.iter().filter(|c| g.cost(**c) > 1).count();
    assert!(muddy <= 1, "the path should go round the mud, not through it: {path:?}");
}

#[test]
fn a_group_crosses_the_map_spreads_out_and_nobody_enters_a_wall() {
    let mut w = World::new(Grid::from_text(MAP, 1.0), kinds());
    let mut ids = Vec::new();
    for k in 0..16 {
        ids.push(w.spawn(1, 0, Pos::new(2.0 + (k % 4) as f32 * 0.9, 3.0 + (k / 4) as f32 * 0.9)));
    }
    let to = Pos::new(15.5, 5.5);
    w.order(&ids, Order::Move(to), false);
    run(&mut w, 25.0);
    for u in &w.units {
        assert!(w.grid.walkable(w.grid.cell_of(u.pos)), "a unit is inside a wall at {:?}", u.pos);
        assert!(u.pos.dist(to) < 4.0, "a unit didn't arrive: {:?}", u.pos);
        assert!(u.orders.is_empty(), "every move finished");
    }
    for (i, a) in w.units.iter().enumerate() {
        for b in &w.units[i + 1..] {
            assert!(a.pos.dist(b.pos) > 0.4, "two units stand on each other");
        }
    }
    assert!(w.flow_fields_kept() <= 16);
}

#[test]
fn queued_orders_run_one_after_another() {
    let mut w = World::new(Grid::new(20, 20, 1.0), kinds());
    let id = w.spawn(1, 0, Pos::new(1.5, 1.5));
    w.order(&[id], Order::Move(Pos::new(10.5, 1.5)), false);
    w.order(&[id], Order::Move(Pos::new(10.5, 10.5)), true);
    run(&mut w, 2.0);
    assert_eq!(w.unit(id).unwrap().orders.len(), 2);
    run(&mut w, 10.0);
    assert!(w.unit(id).unwrap().pos.dist(Pos::new(10.5, 10.5)) < 0.5);
    assert!(w.take_events().contains(&Event::Arrived { id }));
}

#[test]
fn soldiers_fight_and_one_dies() {
    let mut w = World::new(Grid::new(20, 20, 1.0), kinds());
    let a = w.spawn(1, 0, Pos::new(2.5, 5.5));
    let b = w.spawn(1, 1, Pos::new(12.5, 5.5));
    // B is wounded, so A wins.
    let bi = w.units.iter().position(|u| u.id == b).unwrap();
    w.units[bi].health = 30.0;
    w.order(&[a], Order::Attack(b), false);
    run(&mut w, 15.0);
    assert!(w.unit(b).is_none(), "the target should be dead");
    assert!(w.unit(a).is_some());
    let ev = w.take_events();
    assert!(ev.iter().any(|e| matches!(e, Event::Hit { by, target, .. } if *by == a && *target == b)));
    assert!(ev.iter().any(|e| matches!(e, Event::Died { id, .. } if *id == b)));
}

#[test]
fn attack_move_stops_to_fight_on_the_way() {
    let mut w = World::new(Grid::new(30, 10, 1.0), kinds());
    let a = w.spawn(1, 0, Pos::new(1.5, 5.5));
    let b = w.spawn(1, 1, Pos::new(10.5, 5.5));
    let bi = w.units.iter().position(|u| u.id == b).unwrap();
    w.units[bi].health = 20.0;
    w.order(&[a], Order::AttackMove(Pos::new(28.5, 5.5)), false);
    run(&mut w, 20.0);
    assert!(w.unit(b).is_none(), "it fought the enemy on the way");
    assert!(w.unit(a).unwrap().pos.dist(Pos::new(28.5, 5.5)) < 0.5, "then went on");
}

#[test]
fn workers_gather_and_bring_it_home() {
    let mut w = World::new(Grid::new(30, 10, 1.0), kinds());
    w.spawn(2, 0, Pos::new(3.0, 5.0));
    let gold = w.add_resource("gold", Pos::new(15.5, 5.5), 40.0);
    let worker = w.spawn(0, 0, Pos::new(5.5, 5.5));
    w.order(&[worker], Order::Gather(gold), false);
    run(&mut w, 60.0);
    assert_eq!(w.players[&0u8].stock.get("gold"), Some(&40), "all the gold came home");
    assert!(w.resources[0].amount <= 0.0);
    assert!(w.unit(worker).unwrap().orders.is_empty(), "it stops when the gold runs out");
    assert!(w.take_events().iter().any(|e| matches!(e, Event::Brought { amount: 10, .. })));
}

#[test]
fn halls_train_workers_with_supply_and_cost() {
    let mut w = World::new(Grid::new(20, 20, 1.0), kinds());
    let hall = w.spawn(2, 0, Pos::new(5.0, 5.0));
    w.player(0).stock.insert("gold".into(), 120);
    assert!(w.train(hall, "Soldier").unwrap_err().contains("can't train"));
    w.train(hall, "Worker").unwrap();
    w.train(hall, "Worker").unwrap();
    assert!(w.train(hall, "Worker").unwrap_err().contains("not enough gold"));
    assert_eq!(w.players[&0u8].stock["gold"], 20);
    w.order(&[hall], Order::Move(Pos::new(12.5, 12.5)), false);
    run(&mut w, 1.0);
    assert_eq!(w.units.len(), 1, "training takes its time");
    run(&mut w, 6.0);
    assert_eq!(w.units.len(), 3, "the hall gives 2 supply: two workers");
    assert_eq!(w.players[&0u8].supply_used, 2);
    run(&mut w, 6.0);
    let workers: Vec<_> = w.units.iter().filter(|u| u.kind == 0).collect();
    assert!(workers.iter().any(|u| u.pos.dist(Pos::new(12.5, 12.5)) < 2.5), "new units go to the rally point");

    // Over the supply cap, training waits and says why.
    w.player(0).stock.insert("gold".into(), 50);
    w.train(hall, "Worker").unwrap();
    w.take_events();
    run(&mut w, 4.0);
    assert_eq!(w.units.len(), 3);
    assert!(w.take_events().iter().any(|e| matches!(e, Event::Cant { why, .. } if why.contains("supply"))));
}

#[test]
fn workers_put_up_buildings_that_block_paths() {
    let mut w = World::new(Grid::new(20, 20, 1.0), kinds());
    let worker = w.spawn(0, 0, Pos::new(2.5, 2.5));
    w.player(0).stock.insert("gold".into(), 30);
    let at = Cell { x: 10, y: 10 };
    w.order(&[worker], Order::Build { kind: "House".into(), at }, false);
    run(&mut w, 15.0);
    let ev = w.take_events();
    assert!(ev.iter().any(|e| matches!(e, Event::Built { kind, .. } if kind == "House")), "{ev:?}");
    assert!(!w.grid.walkable(at), "units walk round buildings");
    assert_eq!(w.players[&0u8].stock["gold"], 0);
    assert_eq!(w.players[&0u8].supply_cap, 5);

    // No money: it says so.
    w.order(&[worker], Order::Build { kind: "House".into(), at: Cell { x: 15, y: 15 } }, false);
    run(&mut w, 10.0);
    assert!(w.take_events().iter().any(|e| matches!(e, Event::Cant { why, .. } if why.contains("not enough gold"))));
}

#[test]
fn fog_hides_what_walls_block_and_remembers_what_was_seen() {
    let g = Grid::from_text(MAP, 1.0);
    assert!(!line_of_sight(&g, Cell { x: 5, y: 5 }, Cell { x: 13, y: 5 }));
    assert!(line_of_sight(&g, Cell { x: 5, y: 0 }, Cell { x: 13, y: 0 }));
    let mut fog = Fog::new(&g);
    fog.reveal(&g, g.centre(Cell { x: 5, y: 5 }), 6.0);
    assert_eq!(fog.sight(Cell { x: 7, y: 5 }), Sight::Now);
    assert_eq!(fog.sight(Cell { x: 11, y: 5 }), Sight::Never, "behind the wall");
    assert_eq!(fog.sight(Cell { x: 19, y: 9 }), Sight::Never, "too far");
    fog.clear_now();
    assert_eq!(fog.sight(Cell { x: 7, y: 5 }), Sight::Before);

    let mut w = World::new(g, kinds());
    w.spawn(1, 0, Pos::new(5.5, 5.5));
    w.step(0.05);
    assert_eq!(w.fog[&0u8].sight(Cell { x: 6, y: 5 }), Sight::Now);
}

#[test]
fn selecting_prefers_units_and_groups_remember() {
    let mut w = World::new(Grid::new(20, 20, 1.0), kinds());
    let hall = w.spawn(2, 0, Pos::new(5.0, 5.0));
    let a = w.spawn(1, 0, Pos::new(7.5, 5.5));
    let b = w.spawn(1, 0, Pos::new(8.5, 5.5));
    let enemy = w.spawn(1, 1, Pos::new(8.0, 6.5));
    let mut s = Selection::default();
    s.drag(&w, 0, Pos::new(3.0, 3.0), Pos::new(10.0, 8.0));
    assert_eq!(s.chosen, vec![a, b], "units, not the hall or the enemy");
    s.drag(&w, 0, Pos::new(4.0, 4.0), Pos::new(6.0, 6.0));
    assert_eq!(s.chosen, vec![hall], "only a building in the box: the building");
    s.click(&w, 0, Pos::new(7.6, 5.5), false);
    assert_eq!(s.chosen, vec![a]);
    s.click(&w, 0, Pos::new(8.5, 5.5), true);
    assert_eq!(s.chosen, vec![a, b]);
    s.set_group(1);
    s.click(&w, 0, Pos::new(15.0, 15.0), false);
    assert!(s.chosen.is_empty());
    let ei = w.units.iter().position(|u| u.id == b).unwrap();
    w.units[ei].health = 0.0;
    w.step(0.05);
    s.recall(&w, 1);
    assert_eq!(s.chosen, vec![a], "the one that died is gone from the group");
    let _ = enemy;
}

#[test]
fn the_same_orders_give_the_same_game() {
    let play = || {
        let mut w = World::new(Grid::from_text(MAP, 1.0), kinds());
        let mut ids = Vec::new();
        for k in 0..10 {
            ids.push(w.spawn(1, (k % 2) as u8, Pos::new(2.0 + k as f32, 1.5 + (k % 3) as f32 * 3.0)));
        }
        let (even, odd): (Vec<u32>, Vec<u32>) = ids.iter().copied().partition(|id| *id % 2 == 1);
        w.order(&even, Order::AttackMove(Pos::new(17.5, 5.5)), false);
        w.order(&odd, Order::AttackMove(Pos::new(1.5, 5.5)), false);
        run(&mut w, 30.0);
        w.units.iter().map(|u| (u.id, u.pos.x.to_bits(), u.pos.y.to_bits(), u.health.to_bits())).collect::<Vec<_>>()
    };
    assert_eq!(play(), play());
}
