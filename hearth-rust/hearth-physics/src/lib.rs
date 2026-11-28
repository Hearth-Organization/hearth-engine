//! Physics for Hearth World entities, with Rapier (pure Rust, Apache-2.0) in PhysX's place. Give an entity a
//! `Body` and a `Shape` and it falls, bounces, stacks and gets in the way; rays find what they hit; touches are
//! reported so the game (and Hearth Script) can react. Z is up.

use std::collections::{HashMap, HashSet};

use glam::{Quat, Vec3};
use hearth_world::{Entity, Parent, Transform, World};
use rapier3d::prelude::{ColliderBuilder, PhysicsWorld, QueryFilter, Ray, RigidBodyBuilder, RigidBodyHandle};
use serde::{Deserialize, Serialize};

type RVec = rapier3d::math::Vector;

fn to_r(v: Vec3) -> RVec {
    RVec::new(v.x, v.y, v.z)
}

fn from_r(v: RVec) -> Vec3 {
    Vec3::new(v.x, v.y, v.z)
}

#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub enum Body {
    /// Moved by physics (falls, gets pushed).
    Dynamic,
    /// Never moves (floors, walls).
    Fixed,
    /// Moved by the game (doors, lifts), pushing dynamic things out of the way.
    Moved,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub enum ShapeKind {
    Ball { radius: f32 },
    /// Half sizes along x, y and z.
    Box { half: Vec3 },
    /// Upright (along Z): the usual shape for a character.
    Capsule { half_height: f32, radius: f32 },
    /// A triangle mesh (for level geometry; fixed bodies only).
    Mesh { vertices: Vec<Vec3>, triangles: Vec<[u32; 3]> },
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Shape {
    pub kind: ShapeKind,
    pub friction: f32,
    /// 0 = no bounce, 1 = bounces back fully.
    pub bounciness: f32,
    /// Only reports touches; nothing collides with it (trigger areas).
    pub sensor: bool,
}

impl Shape {
    pub fn new(kind: ShapeKind) -> Shape {
        Shape { kind, friction: 0.6, bounciness: 0.0, sensor: false }
    }
}

/// Something touched something (or stopped touching).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Touch {
    Started(Entity, Entity),
    Stopped(Entity, Entity),
}

pub struct Physics {
    pub world: PhysicsWorld,
    bodies: HashMap<Entity, RigidBodyHandle>,
    by_handle: HashMap<RigidBodyHandle, Entity>,
    touching: HashSet<(Entity, Entity)>,
    /// Steps a second (fixed, so the game behaves the same every time).
    pub rate: f32,
    leftover: f32,
    pub touches: Vec<Touch>,
}

impl Default for Physics {
    fn default() -> Physics {
        Physics::new()
    }
}

fn pair(a: Entity, b: Entity) -> (Entity, Entity) {
    if a <= b {
        (a, b)
    } else {
        (b, a)
    }
}

impl Physics {
    pub fn new() -> Physics {
        let mut world = PhysicsWorld::new();
        world.gravity = RVec::new(0.0, 0.0, -9.81);
        Physics { world, bodies: HashMap::new(), by_handle: HashMap::new(), touching: HashSet::new(), rate: 60.0, leftover: 0.0, touches: Vec::new() }
    }

    pub fn set_gravity(&mut self, g: Vec3) {
        self.world.gravity = to_r(g);
    }

    fn add(&mut self, w: &World, e: Entity) -> Result<(), String> {
        let (Some(body), Some(shape)) = (w.get::<Body>(e), w.get::<Shape>(e)) else { return Ok(()) };
        let t = w.get::<Transform>(e).copied().unwrap_or_default();
        let builder = match body {
            Body::Dynamic => RigidBodyBuilder::dynamic(),
            Body::Fixed => RigidBodyBuilder::fixed(),
            Body::Moved => RigidBodyBuilder::kinematic_position_based(),
        };
        let handle = self.world.insert_body(builder.translation(to_r(t.translation)).ccd_enabled(*body == Body::Dynamic).build());
        if let Some(b) = self.world.bodies.get_mut(handle) {
            let q = t.rotation;
            b.set_rotation(rapier3d::math::Rotation::from_xyzw(q.x, q.y, q.z, q.w), false);
        }
        let s = t.scale;
        let collider = match &shape.kind {
            ShapeKind::Ball { radius } => ColliderBuilder::ball(radius * s.max_element()),
            ShapeKind::Box { half } => ColliderBuilder::cuboid(half.x * s.x, half.y * s.y, half.z * s.z),
            ShapeKind::Capsule { half_height, radius } => ColliderBuilder::capsule_z(half_height * s.z, radius * s.x.max(s.y)),
            ShapeKind::Mesh { vertices, triangles } => {
                if *body != Body::Fixed {
                    return Err("a mesh shape can only be on a fixed body (use boxes, balls or capsules for things that move)".into());
                }
                ColliderBuilder::trimesh(vertices.iter().map(|v| to_r(*v * s)).collect(), triangles.clone()).map_err(|e| format!("the mesh shape is broken: {e:?}"))?
            }
        };
        self.world.insert_collider(collider.friction(shape.friction).restitution(shape.bounciness).sensor(shape.sensor).build(), Some(handle));
        self.bodies.insert(e, handle);
        self.by_handle.insert(handle, e);
        Ok(())
    }

    /// Brings physics in step with the world: new bodies in, removed ones out, moved bodies to their transforms.
    pub fn sync(&mut self, w: &World) -> Vec<String> {
        let mut problems = Vec::new();
        let gone: Vec<Entity> = self.bodies.keys().copied().filter(|&e| !w.is_alive(e) || !w.has::<Body>(e)).collect();
        for e in gone {
            if let Some(h) = self.bodies.remove(&e) {
                self.by_handle.remove(&h);
                self.world.remove_body(h);
            }
        }
        let wanted: Vec<Entity> = w.with_both::<Body, Shape>().into_iter().filter(|e| !self.bodies.contains_key(e)).collect();
        for e in wanted {
            if w.has::<Parent>(e) {
                problems.push(format!("{e:?}: a physics body can't have a parent (it moves on its own)"));
                continue;
            }
            if let Err(p) = self.add(w, e) {
                problems.push(format!("{e:?}: {p}"));
            }
        }
        for (e, h) in &self.bodies {
            if w.get::<Body>(*e) == Some(&Body::Moved) {
                if let (Some(t), Some(b)) = (w.get::<Transform>(*e), self.world.bodies.get_mut(*h)) {
                    let q = t.rotation;
                    b.set_next_kinematic_position(rapier3d::math::Pose::from_parts(to_r(t.translation), rapier3d::math::Rotation::from_xyzw(q.x, q.y, q.z, q.w)));
                }
            }
        }
        problems
    }

    /// Moves physics on by `dt` (in fixed steps) and writes where dynamic things ended up back to their transforms.
    pub fn step(&mut self, w: &mut World, dt: f32) -> Vec<String> {
        let problems = self.sync(w);
        let step = 1.0 / self.rate.max(1.0);
        self.world.integration_parameters.dt = step;
        self.leftover += dt;
        let mut steps = 0;
        while self.leftover >= step && steps < 8 {
            self.world.step();
            self.leftover -= step;
            steps += 1;
        }
        if steps == 8 {
            self.leftover = 0.0; // too slow to keep up: drop the backlog instead of freezing
        }
        for (e, h) in &self.bodies {
            if w.get::<Body>(*e) != Some(&Body::Dynamic) {
                continue;
            }
            if let (Some(b), Some(t)) = (self.world.bodies.get(*h), w.get_mut::<Transform>(*e)) {
                t.translation = from_r(b.translation());
                let r = b.rotation();
                t.rotation = Quat::from_xyzw(r.x, r.y, r.z, r.w);
            }
        }
        // Touches that started and stopped.
        let mut now = HashSet::new();
        for p in self.world.contact_pairs() {
            if !p.has_any_active_contact() {
                continue;
            }
            let (a, b) = (self.world.colliders.get(p.collider1).and_then(|c| c.parent()), self.world.colliders.get(p.collider2).and_then(|c| c.parent()));
            if let (Some(a), Some(b)) = (a.and_then(|h| self.by_handle.get(&h)), b.and_then(|h| self.by_handle.get(&h))) {
                now.insert(pair(*a, *b));
            }
        }
        for (c1, c2, intersecting) in self.world.narrow_phase.intersection_pairs() {
            if !intersecting {
                continue;
            }
            let a = self.world.colliders.get(c1).and_then(|c| c.parent()).and_then(|h| self.by_handle.get(&h));
            let b = self.world.colliders.get(c2).and_then(|c| c.parent()).and_then(|h| self.by_handle.get(&h));
            if let (Some(a), Some(b)) = (a, b) {
                now.insert(pair(*a, *b));
            }
        }
        let mut started: Vec<_> = now.difference(&self.touching).copied().collect();
        let mut stopped: Vec<_> = self.touching.difference(&now).copied().collect();
        started.sort();
        stopped.sort();
        self.touches.extend(started.into_iter().map(|(a, b)| Touch::Started(a, b)));
        self.touches.extend(stopped.into_iter().map(|(a, b)| Touch::Stopped(a, b)));
        self.touching = now;
        problems
    }

    /// What a ray from `from` along `dir` hits first, and how far away (within `max`).
    pub fn ray(&self, from: Vec3, dir: Vec3, max: f32, ignore: Option<Entity>) -> Option<(Entity, f32)> {
        self.ray_past(from, dir, max, ignore.as_slice())
    }

    /// A ray that passes through the given entities.
    pub fn ray_past(&self, from: Vec3, dir: Vec3, max: f32, ignore: &[Entity]) -> Option<(Entity, f32)> {
        let skip: Vec<_> = ignore.iter().filter_map(|e| self.bodies.get(e).copied()).collect();
        let pred = |_: rapier3d::geometry::ColliderHandle, c: &rapier3d::geometry::Collider| c.parent().is_none_or(|p| !skip.contains(&p));
        let filter = QueryFilter::default().predicate(&pred);
        let q = self.world.query_pipeline_with_filter(filter);
        let (collider, toi) = q.cast_ray(&Ray::new(to_r(from), to_r(dir.normalize_or_zero())), max, true)?;
        let parent = self.world.colliders.get(collider)?.parent()?;
        Some((*self.by_handle.get(&parent)?, toi))
    }

    /// Pushes a dynamic thing (an explosion, a kick).
    pub fn push(&mut self, e: Entity, impulse: Vec3) {
        if let Some(b) = self.bodies.get(&e).and_then(|h| self.world.bodies.get_mut(*h)) {
            b.apply_impulse(to_r(impulse), true);
        }
    }

    pub fn velocity(&self, e: Entity) -> Option<Vec3> {
        self.bodies.get(&e).and_then(|h| self.world.bodies.get(*h)).map(|b| from_r(b.linvel()))
    }

    pub fn take_touches(&mut self) -> Vec<Touch> {
        std::mem::take(&mut self.touches)
    }
}
