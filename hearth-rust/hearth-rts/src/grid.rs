//! The map, as a grid of cells: what can be walked on and how slow it is.

/// A place on the map, in cells.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Cell {
    pub x: i32,
    pub y: i32,
}

#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct Pos {
    pub x: f32,
    pub y: f32,
}

impl Pos {
    pub fn new(x: f32, y: f32) -> Pos {
        Pos { x, y }
    }
    pub fn dist(self, o: Pos) -> f32 {
        ((self.x - o.x).powi(2) + (self.y - o.y).powi(2)).sqrt()
    }
    pub fn plus(self, o: Pos) -> Pos {
        Pos::new(self.x + o.x, self.y + o.y)
    }
    pub fn minus(self, o: Pos) -> Pos {
        Pos::new(self.x - o.x, self.y - o.y)
    }
    pub fn scale(self, s: f32) -> Pos {
        Pos::new(self.x * s, self.y * s)
    }
    pub fn length(self) -> f32 {
        (self.x * self.x + self.y * self.y).sqrt()
    }
    pub fn normalized(self) -> Pos {
        let l = self.length();
        if l > 1e-6 {
            self.scale(1.0 / l)
        } else {
            Pos::default()
        }
    }
}

/// 0 = can't walk; 1 = normal ground; more = slower (mud 3, shallow water 5).
pub type Cost = u8;
pub const BLOCKED: Cost = 0;

#[derive(Clone, Debug)]
pub struct Grid {
    pub width: i32,
    pub height: i32,
    /// Metres per cell side.
    pub cell_size: f32,
    cost: Vec<Cost>,
}

impl Grid {
    pub fn new(width: i32, height: i32, cell_size: f32) -> Grid {
        Grid { width, height, cell_size, cost: vec![1; (width.max(0) * height.max(0)) as usize] }
    }

    /// A grid from rows of text: `#` wall, `~` water (5), `,` mud (3), anything else ground.
    pub fn from_text(rows: &str, cell_size: f32) -> Grid {
        let lines: Vec<&str> = rows.lines().map(str::trim_end).filter(|l| !l.is_empty()).collect();
        let width = lines.iter().map(|l| l.chars().count()).max().unwrap_or(0) as i32;
        let mut g = Grid::new(width, lines.len() as i32, cell_size);
        for (y, l) in lines.iter().enumerate() {
            for (x, ch) in l.chars().enumerate() {
                let c = match ch {
                    '#' => BLOCKED,
                    '~' => 5,
                    ',' => 3,
                    _ => 1,
                };
                g.set(Cell { x: x as i32, y: y as i32 }, c);
            }
        }
        g
    }

    pub fn inside(&self, c: Cell) -> bool {
        c.x >= 0 && c.y >= 0 && c.x < self.width && c.y < self.height
    }

    fn at(&self, c: Cell) -> usize {
        (c.y * self.width + c.x) as usize
    }

    pub fn cost(&self, c: Cell) -> Cost {
        if self.inside(c) {
            self.cost[self.at(c)]
        } else {
            BLOCKED
        }
    }

    pub fn set(&mut self, c: Cell, cost: Cost) {
        if self.inside(c) {
            let i = self.at(c);
            self.cost[i] = cost;
        }
    }

    pub fn walkable(&self, c: Cell) -> bool {
        self.cost(c) != BLOCKED
    }

    pub fn cell_of(&self, p: Pos) -> Cell {
        Cell { x: (p.x / self.cell_size).floor() as i32, y: (p.y / self.cell_size).floor() as i32 }
    }

    pub fn centre(&self, c: Cell) -> Pos {
        Pos::new((c.x as f32 + 0.5) * self.cell_size, (c.y as f32 + 0.5) * self.cell_size)
    }

    pub fn index(&self, c: Cell) -> Option<usize> {
        self.inside(c).then_some(self.at(c))
    }

    pub fn cells(&self) -> usize {
        self.cost.len()
    }

    /// The nearest cell a unit can stand on (for an order given on a wall).
    pub fn nearest_walkable(&self, c: Cell) -> Option<Cell> {
        if self.walkable(c) {
            return Some(c);
        }
        for r in 1..self.width.max(self.height) {
            let mut best: Option<(i32, Cell)> = None;
            for dy in -r..=r {
                for dx in -r..=r {
                    if dx.abs() != r && dy.abs() != r {
                        continue;
                    }
                    let n = Cell { x: c.x + dx, y: c.y + dy };
                    let d = dx * dx + dy * dy;
                    if self.walkable(n) && best.map(|(bd, _)| d < bd).unwrap_or(true) {
                        best = Some((d, n));
                    }
                }
            }
            if let Some((_, n)) = best {
                return Some(n);
            }
        }
        None
    }
}
