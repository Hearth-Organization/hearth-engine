//! Entities and components: the things in a level (an entity is just an id) and what they have (components: a
//! name, a transform, a mesh, a light, a script...). Systems run each frame in the order they were added.
//! Parents and children move together. Levels are saved and loaded as JSON (atomic writes, with a backup).
//!
//! This is the Rust rewrite of O3DE's AzFramework entities and components.

pub mod scene;

use std::any::{Any, TypeId};
use std::collections::HashMap;

use glam::{Mat4, Quat, Vec3};
use serde::{Deserialize, Serialize};

/// An entity: its slot and which use of that slot (so an old id never points at a new entity).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
pub struct Entity {
    pub index: u32,
    pub generation: u32,
}

trait AnyStorage: Any {
    fn remove_entity(&mut self, e: Entity);
    fn as_any(&self) -> &dyn Any;
    fn as_any_mut(&mut self) -> &mut dyn Any;
}

/// One component type's values, packed together (fast to go through), with a lookup by entity.
struct Storage<T> {
    values: Vec<T>,
    owners: Vec<Entity>,
    slot: Vec<Option<usize>>,
}

impl<T: 'static> Storage<T> {
    fn new() -> Storage<T> {
        Storage { values: Vec::new(), owners: Vec::new(), slot: Vec::new() }
    }
    fn index(&self, e: Entity) -> Option<usize> {
        let i = (*self.slot.get(e.index as usize)?)?;
        (self.owners[i] == e).then_some(i)
    }
    fn insert(&mut self, e: Entity, v: T) {
        if let Some(i) = self.index(e) {
            self.values[i] = v;
            return;
        }
        if self.slot.len() <= e.index as usize {
            self.slot.resize(e.index as usize + 1, None);
        }
        self.slot[e.index as usize] = Some(self.values.len());
        self.values.push(v);
        self.owners.push(e);
    }
    fn take(&mut self, e: Entity) -> Option<T> {
        let i = self.index(e)?;
        self.slot[e.index as usize] = None;
        let last = self.values.len() - 1;
        if i != last {
            let moved = self.owners[last];
            self.slot[moved.index as usize] = Some(i);
        }
        self.owners.swap_remove(i);
        Some(self.values.swap_remove(i))
    }
}

impl<T: 'static> AnyStorage for Storage<T> {
    fn remove_entity(&mut self, e: Entity) {
        self.take(e);
    }
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Name(pub String);

/// Where a thing is, relative to its parent (or the world).
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
pub struct Transform {
    pub translation: Vec3,
    pub rotation: Quat,
    pub scale: Vec3,
}

impl Default for Transform {
    fn default() -> Transform {
        Transform { translation: Vec3::ZERO, rotation: Quat::IDENTITY, scale: Vec3::ONE }
    }
}

impl Transform {
    pub fn at(translation: Vec3) -> Transform {
        Transform { translation, ..Transform::default() }
    }
    pub fn matrix(&self) -> Mat4 {
        Mat4::from_scale_rotation_translation(self.scale, self.rotation, self.translation)
    }
}

/// Where a thing is in the world (worked out from the transforms up its parents).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GlobalTransform(pub Mat4);

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Parent(pub Entity);

#[derive(Clone, Debug, PartialEq, Default)]
pub struct Children(pub Vec<Entity>);

/// Frame time, for systems.
#[derive(Clone, Copy, Debug, Default)]
pub struct Time {
    pub delta: f32,
    pub elapsed: f64,
    pub frame: u64,
}

#[derive(Default)]
pub struct World {
    generations: Vec<u32>,
    alive: Vec<bool>,
    free: Vec<u32>,
    storages: HashMap<TypeId, Box<dyn AnyStorage>>,
    resources: HashMap<TypeId, Box<dyn Any>>,
}

impl World {
    pub fn new() -> World {
        World::default()
    }

    pub fn spawn(&mut self) -> Entity {
        if let Some(i) = self.free.pop() {
            self.alive[i as usize] = true;
            return Entity { index: i, generation: self.generations[i as usize] };
        }
        self.generations.push(0);
        self.alive.push(true);
        Entity { index: (self.generations.len() - 1) as u32, generation: 0 }
    }

    /// A new entity with a name and a transform.
    pub fn spawn_named(&mut self, name: &str, t: Transform) -> Entity {
        let e = self.spawn();
        self.insert(e, Name(name.into()));
        self.insert(e, t);
        e
    }

    pub fn is_alive(&self, e: Entity) -> bool {
        self.alive.get(e.index as usize).copied().unwrap_or(false) && self.generations[e.index as usize] == e.generation
    }

    /// Removes an entity, its components and its children (and theirs).
    pub fn despawn(&mut self, e: Entity) {
        if !self.is_alive(e) {
            return;
        }
        if let Some(children) = self.get::<Children>(e).cloned() {
            for c in children.0 {
                self.despawn(c);
            }
        }
        if let Some(Parent(p)) = self.get::<Parent>(e).copied() {
            if let Some(ch) = self.get_mut::<Children>(p) {
                ch.0.retain(|&c| c != e);
            }
        }
        for s in self.storages.values_mut() {
            s.remove_entity(e);
        }
        let i = e.index as usize;
        self.alive[i] = false;
        self.generations[i] = self.generations[i].wrapping_add(1);
        self.free.push(e.index);
    }

    fn storage<T: 'static>(&self) -> Option<&Storage<T>> {
        self.storages.get(&TypeId::of::<T>()).and_then(|s| s.as_any().downcast_ref())
    }

    fn storage_mut<T: 'static>(&mut self) -> &mut Storage<T> {
        self.storages
            .entry(TypeId::of::<T>())
            .or_insert_with(|| Box::new(Storage::<T>::new()))
            .as_any_mut()
            .downcast_mut()
            .expect("a storage holds its own type")
    }

    /// Gives an entity a component (replacing one of the same type). Does nothing for a removed entity.
    pub fn insert<T: 'static>(&mut self, e: Entity, v: T) {
        if self.is_alive(e) {
            self.storage_mut::<T>().insert(e, v);
        }
    }

    pub fn remove<T: 'static>(&mut self, e: Entity) -> Option<T> {
        self.storage_mut::<T>().take(e)
    }

    pub fn get<T: 'static>(&self, e: Entity) -> Option<&T> {
        let s = self.storage::<T>()?;
        s.index(e).map(|i| &s.values[i])
    }

    pub fn get_mut<T: 'static>(&mut self, e: Entity) -> Option<&mut T> {
        let s = self.storage_mut::<T>();
        s.index(e).map(move |i| &mut s.values[i])
    }

    pub fn has<T: 'static>(&self, e: Entity) -> bool {
        self.get::<T>(e).is_some()
    }

    /// Every entity with a T, and its T.
    pub fn query<T: 'static>(&self) -> impl Iterator<Item = (Entity, &T)> {
        self.storage::<T>().into_iter().flat_map(|s| s.owners.iter().copied().zip(s.values.iter()))
    }

    /// Changes every T in place.
    pub fn for_each_mut<T: 'static>(&mut self, mut f: impl FnMut(Entity, &mut T)) {
        let s = self.storage_mut::<T>();
        for (e, v) in s.owners.iter().copied().zip(s.values.iter_mut()) {
            f(e, v);
        }
    }

    /// The entities that have both an A and a B.
    pub fn with_both<A: 'static, B: 'static>(&self) -> Vec<Entity> {
        self.query::<A>().map(|(e, _)| e).filter(|&e| self.has::<B>(e)).collect()
    }

    pub fn count<T: 'static>(&self) -> usize {
        self.storage::<T>().map(|s| s.values.len()).unwrap_or(0)
    }

    pub fn find(&self, name: &str) -> Option<Entity> {
        let mut found: Vec<Entity> = self.query::<Name>().filter(|(_, n)| n.0 == name).map(|(e, _)| e).collect();
        found.sort();
        found.first().copied()
    }

    /// Makes `child` a child of `parent` (or of nothing), keeping both sides in step. Refuses loops.
    pub fn set_parent(&mut self, child: Entity, parent: Option<Entity>) -> Result<(), String> {
        if !self.is_alive(child) {
            return Err("that entity is gone".into());
        }
        if let Some(p) = parent {
            if !self.is_alive(p) {
                return Err("the parent is gone".into());
            }
            let mut up = Some(p);
            while let Some(a) = up {
                if a == child {
                    return Err("that would make a thing its own parent".into());
                }
                up = self.get::<Parent>(a).map(|p| p.0);
            }
        }
        if let Some(Parent(old)) = self.remove::<Parent>(child) {
            if let Some(ch) = self.get_mut::<Children>(old) {
                ch.0.retain(|&c| c != child);
            }
        }
        if let Some(p) = parent {
            self.insert(child, Parent(p));
            if !self.has::<Children>(p) {
                self.insert(p, Children::default());
            }
            if let Some(ch) = self.get_mut::<Children>(p) {
                ch.0.push(child);
            }
        }
        Ok(())
    }

    /// Works out every entity's place in the world from its transform and its parents'.
    pub fn update_transforms(&mut self) {
        let roots: Vec<Entity> = self.query::<Transform>().map(|(e, _)| e).filter(|&e| !self.has::<Parent>(e)).collect();
        let mut todo: Vec<(Entity, Mat4)> = roots.into_iter().map(|e| (e, Mat4::IDENTITY)).collect();
        while let Some((e, up)) = todo.pop() {
            let local = self.get::<Transform>(e).map(|t| t.matrix()).unwrap_or(Mat4::IDENTITY);
            let world = up * local;
            self.insert(e, GlobalTransform(world));
            if let Some(ch) = self.get::<Children>(e) {
                todo.extend(ch.0.iter().map(|&c| (c, world)));
            }
        }
    }

    pub fn insert_resource<T: 'static>(&mut self, v: T) {
        self.resources.insert(TypeId::of::<T>(), Box::new(v));
    }

    pub fn resource<T: 'static>(&self) -> Option<&T> {
        self.resources.get(&TypeId::of::<T>()).and_then(|r| r.downcast_ref())
    }

    pub fn resource_mut<T: 'static>(&mut self) -> Option<&mut T> {
        self.resources.get_mut(&TypeId::of::<T>()).and_then(|r| r.downcast_mut())
    }

    pub fn entities(&self) -> Vec<Entity> {
        (0..self.alive.len()).filter(|&i| self.alive[i]).map(|i| Entity { index: i as u32, generation: self.generations[i] }).collect()
    }
}

type System = Box<dyn FnMut(&mut World)>;

/// The systems that run each frame, in the order they were added.
#[derive(Default)]
pub struct Schedule {
    systems: Vec<(String, System)>,
}

impl Schedule {
    pub fn add(&mut self, name: &str, f: impl FnMut(&mut World) + 'static) -> &mut Schedule {
        self.systems.push((name.to_string(), Box::new(f)));
        self
    }

    pub fn names(&self) -> Vec<&str> {
        self.systems.iter().map(|(n, _)| n.as_str()).collect()
    }

    /// One frame: moves time on, runs every system, then works out where everything is.
    pub fn run(&mut self, world: &mut World, delta: f32) {
        let t = world.resource::<Time>().copied().unwrap_or_default();
        world.insert_resource(Time { delta, elapsed: t.elapsed + delta as f64, frame: t.frame + 1 });
        for (_, s) in &mut self.systems {
            s(world);
        }
        world.update_transforms();
    }
}
