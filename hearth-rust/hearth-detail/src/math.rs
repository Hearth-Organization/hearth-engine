//! Small vector maths and bounding spheres.

pub type V3 = [f32; 3];

pub fn sub(a: V3, b: V3) -> V3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

pub fn add(a: V3, b: V3) -> V3 {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}

pub fn scale(a: V3, s: f32) -> V3 {
    [a[0] * s, a[1] * s, a[2] * s]
}

pub fn cross(a: V3, b: V3) -> V3 {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}

pub fn dot(a: V3, b: V3) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

pub fn length(a: V3) -> f32 {
    dot(a, a).sqrt()
}

pub fn dist2(a: V3, b: V3) -> f32 {
    let d = sub(a, b);
    dot(d, d)
}

/// A ball that holds something: used to skip pieces that are off screen and to judge how big a piece's error looks.
#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct Sphere {
    pub center: V3,
    pub radius: f32,
}

impl Sphere {
    /// A ball around points (Ritter's method: close to the smallest, and quick).
    pub fn of_points(points: impl Iterator<Item = V3> + Clone) -> Sphere {
        let mut it = points.clone();
        let Some(first) = it.next() else { return Sphere::default() };
        let far = |from: V3| points.clone().fold(from, |best, p| if dist2(p, from) > dist2(best, from) { p } else { best });
        let a = far(first);
        let b = far(a);
        let mut s = Sphere { center: scale(add(a, b), 0.5), radius: dist2(a, b).sqrt() * 0.5 };
        for p in points {
            let d = dist2(p, s.center).sqrt();
            if d > s.radius {
                let r = (s.radius + d) * 0.5;
                s.center = add(s.center, scale(sub(p, s.center), (r - s.radius) / d));
                s.radius = r;
            }
        }
        s
    }

    /// The smallest ball that holds both.
    pub fn merge(self, o: Sphere) -> Sphere {
        let d = dist2(self.center, o.center).sqrt();
        if d + o.radius <= self.radius {
            return self;
        }
        if d + self.radius <= o.radius {
            return o;
        }
        let r = (d + self.radius + o.radius) * 0.5;
        let center = if d > 0.0 { add(self.center, scale(sub(o.center, self.center), (r - self.radius) / d)) } else { self.center };
        Sphere { center, radius: r }
    }
}

/// A position along a curve that keeps nearby things together (for compact pieces).
pub fn morton(p: V3, min: V3, size: V3) -> u32 {
    fn spread(mut x: u32) -> u32 {
        x &= 0x3ff;
        x = (x | (x << 16)) & 0x030000ff;
        x = (x | (x << 8)) & 0x0300f00f;
        x = (x | (x << 4)) & 0x030c30c3;
        (x | (x << 2)) & 0x09249249
    }
    let q = |i: usize| (((p[i] - min[i]) / size[i].max(1e-20)).clamp(0.0, 1.0) * 1023.0) as u32;
    spread(q(0)) | (spread(q(1)) << 1) | (spread(q(2)) << 2)
}

pub fn bbox(points: impl Iterator<Item = V3>) -> (V3, V3) {
    let mut lo = [f32::MAX; 3];
    let mut hi = [f32::MIN; 3];
    for p in points {
        lo = [lo[0].min(p[0]), lo[1].min(p[1]), lo[2].min(p[2])];
        hi = [hi[0].max(p[0]), hi[1].max(p[1]), hi[2].max(p[2])];
    }
    (lo, sub(hi, lo))
}
