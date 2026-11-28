use std::collections::{HashSet, VecDeque};

use hearth_levelgen::{blocked, cell_chance, chunk, zone_value, Settings, Wall};

#[test]
fn the_same_seed_gives_the_same_world_and_another_seed_doesnt() {
    let s = Settings::default();
    assert_eq!(chunk(&s, 3, -7), chunk(&s, 3, -7));
    let other = Settings { seed: 2, ..Settings::default() };
    let differs = (0..10).any(|i| chunk(&s, i, 0).walls != chunk(&other, i, 0).walls);
    assert!(differs);
}

#[test]
fn every_border_has_at_least_two_doorways() {
    let s = Settings { border_open: 0.0, ..Settings::default() };
    for cy in -5..5 {
        for cx in -5..5 {
            let c = chunk(&s, cx, cy);
            let south = c.doorways.iter().filter(|d| d.along_x && d.y == cy * s.chunk_cells).count();
            let west = c.doorways.iter().filter(|d| !d.along_x && d.x == cx * s.chunk_cells).count();
            assert_eq!((south, west), (2, 2), "chunk {cx},{cy}");
        }
    }
}

#[test]
fn a_big_area_is_one_connected_place_with_no_wall_built_twice() {
    for seed in 1..6 {
        let s = Settings { seed, open: 0.0, border_open: 0.0, room_chance: 0.0, ..Settings::default() };
        let n = s.chunk_cells;
        let (lo, hi) = (-2 * n, 2 * n - 1); // a 4 x 4 chunk area
        let mut seen = HashSet::new();
        let mut todo = VecDeque::from([(lo, lo)]);
        seen.insert((lo, lo));
        while let Some((x, y)) = todo.pop_front() {
            for (dx, dy) in [(1, 0), (-1, 0), (0, 1), (0, -1)] {
                let (nx, ny) = (x + dx, y + dy);
                if nx >= lo && ny >= lo && nx <= hi && ny <= hi && !blocked(&s, x, y, dx, dy) && seen.insert((nx, ny)) {
                    todo.push_back((nx, ny));
                }
            }
        }
        assert_eq!(seen.len(), ((hi - lo + 1) * (hi - lo + 1)) as usize, "seed {seed}: someone is walled in");

        let mut all: Vec<Wall> = Vec::new();
        for cy in -2..2 {
            for cx in -2..2 {
                all.extend(chunk(&s, cx, cy).walls);
            }
        }
        let unique: HashSet<Wall> = all.iter().copied().collect();
        assert_eq!(unique.len(), all.len(), "a wall was built twice");
    }
}

#[test]
fn a_pure_maze_has_exactly_the_walls_a_maze_needs_and_rooms_are_open() {
    let s = Settings { open: 0.0, border_open: 0.0, room_chance: 0.0, ..Settings::default() };
    let n = s.chunk_cells;
    let c = chunk(&s, 0, 0);
    let inside = c.walls.iter().filter(|w| (w.along_x && w.y != 0) || (!w.along_x && w.x != 0)).count() as i32;
    // Every inside edge is a wall except the n*n - 1 a tree through every cell needs.
    assert_eq!(inside, 2 * n * (n - 1) - (n * n - 1));

    let rooms = Settings { room_chance: 1.0, ..s };
    let c = chunk(&rooms, 1, 1);
    let r = c.room.expect("a room");
    for w in &c.walls {
        let inside_room = if w.along_x {
            w.x >= r.x && w.x < r.x + r.w && w.y > r.y && w.y < r.y + r.h
        } else {
            w.y >= r.y && w.y < r.y + r.h && w.x > r.x && w.x < r.x + r.w
        };
        assert!(!inside_room, "a wall inside the room: {w:?} in {r:?}");
    }
}

#[test]
fn places_and_zones_are_steady() {
    let s = Settings::default();
    let lights = (0..400).filter(|i| cell_chance(&s, i % 20, i / 20, "light", 0.3)).count();
    assert!((80..160).contains(&lights), "about 30% of cells: {lights}");
    assert_eq!(cell_chance(&s, 5, 5, "light", 0.3), cell_chance(&s, 5, 5, "light", 0.3));
    let mut last = zone_value(&s, 0.0, 0.0, 4.0, 1);
    for i in 1..200 {
        let v = zone_value(&s, i as f32 * 0.05, 0.0, 4.0, 1);
        assert!((0.0..=1.0).contains(&v));
        assert!((v - last).abs() < 0.1, "zones change smoothly");
        last = v;
    }
}
