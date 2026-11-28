//! `hearth-levelgen area SEED minX minY maxX maxY`: the walls of an area (in cells) as JSON, for the Hub's Level
//! Generator view. `hearth-levelgen check SEED`: walks a big area and says whether every cell can be reached.

use hearth_levelgen::{blocked, chunk, wall_box, Settings};

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    let num = |i: usize| a.get(i).and_then(|v| v.parse::<i64>().ok());
    match (a.first().map(String::as_str), num(1)) {
        (Some("area"), Some(seed)) => {
            let s = Settings { seed: seed as u64, ..Settings::default() };
            let (x0, y0, x1, y1) = (num(2).unwrap_or(-12) as i32, num(3).unwrap_or(-12) as i32, num(4).unwrap_or(12) as i32, num(5).unwrap_or(12) as i32);
            let n = s.chunk_cells;
            let mut walls = Vec::new();
            for cy in y0.div_euclid(n)..=y1.div_euclid(n) + 1 {
                for cx in x0.div_euclid(n)..=x1.div_euclid(n) + 1 {
                    for w in chunk(&s, cx, cy).walls {
                        if w.x >= x0 && w.x <= x1 && w.y >= y0 && w.y <= y1 {
                            walls.push(serde_json::json!(wall_box(&s, &w, 0.2)));
                        }
                    }
                }
            }
            println!("{}", serde_json::json!({ "seed": seed, "cell_size": s.cell_size, "walls": walls }));
        }
        (Some("check"), Some(seed)) => {
            let s = Settings { seed: seed as u64, ..Settings::default() };
            let r = 30;
            let mut seen = std::collections::HashSet::new();
            let mut todo = vec![(0, 0)];
            seen.insert((0, 0));
            while let Some((x, y)) = todo.pop() {
                for (dx, dy) in [(1, 0), (-1, 0), (0, 1), (0, -1)] {
                    let (nx, ny) = (x + dx, y + dy);
                    if nx.abs() <= r && ny.abs() <= r && !blocked(&s, x, y, dx, dy) && seen.insert((nx, ny)) {
                        todo.push((nx, ny));
                    }
                }
            }
            let all = ((2 * r + 1) * (2 * r + 1)) as usize;
            println!("{} of {all} cells reachable from the start", seen.len());
        }
        _ => {
            eprintln!("usage: hearth-levelgen area SEED [minX minY maxX maxY] | hearth-levelgen check SEED");
            std::process::exit(2);
        }
    }
}
