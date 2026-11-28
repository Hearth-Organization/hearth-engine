use hearth_levelgen::wfc::{generate, Rules};

const ROOMS: &str = "
##########
#....#...#
#....D...#
#....#...#
###D######
#........#
#........#
##########
";

fn check_follows_rules(r: &Rules, map: &[Vec<char>]) {
    for (y, row) in map.iter().enumerate() {
        for (x, &a) in row.iter().enumerate() {
            if x + 1 < row.len() {
                assert!(r.may(a, 0, row[x + 1]), "'{a}' then '{}' at {x},{y} never happens in the example", row[x + 1]);
            }
            if y + 1 < map.len() {
                assert!(r.may(a, 3, map[y + 1][x]), "'{a}' above '{}' at {x},{y} never happens in the example", map[y + 1][x]);
            }
        }
    }
}

#[test]
fn a_new_map_follows_the_example_and_is_the_same_for_a_seed() {
    let r = Rules::learn(ROOMS, true).unwrap();
    assert_eq!(r.tiles, ['#', '.', 'D']);
    let map = generate(&r, 30, 20, 7, &[]).unwrap();
    assert_eq!((map[0].len(), map.len()), (30, 20));
    check_follows_rules(&r, &map);
    assert_eq!(map, generate(&r, 30, 20, 7, &[]).unwrap());
    let other = generate(&r, 30, 20, 8, &[]).unwrap();
    assert_ne!(map, other, "another seed, another map");
    let floor = map.iter().flatten().filter(|&&c| c == '.').count();
    assert!(floor > 100, "mostly rooms, like the example: {floor} floor cells");
}

#[test]
fn pinned_cells_stay_and_bad_pins_are_explained() {
    let r = Rules::learn(ROOMS, true).unwrap();
    let pins: Vec<(usize, usize, char)> = (0..20).map(|x| (x, 0, '#')).chain([(5, 5, '.')]).collect();
    let map = generate(&r, 20, 12, 3, &pins).unwrap();
    assert!(map[0].iter().all(|&c| c == '#'));
    assert_eq!(map[5][5], '.');
    check_follows_rules(&r, &map);
    assert!(generate(&r, 5, 5, 1, &[(9, 9, '#')]).unwrap_err().contains("outside"));
    assert!(generate(&r, 5, 5, 1, &[(0, 0, 'X')]).unwrap_err().contains("isn't in the example"));
    // A door can never touch a door in the example: pinning two side by side can't work.
    assert!(generate(&r, 6, 6, 1, &[(2, 2, 'D'), (3, 2, 'D')]).unwrap_err().contains("couldn't build"));
}

#[test]
fn bad_examples_are_explained() {
    assert!(Rules::learn("", false).unwrap_err().contains("empty"));
    assert!(Rules::learn("###\n##\n", false).unwrap_err().contains("same length"));
}
