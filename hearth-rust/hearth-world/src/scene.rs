//! Levels saved and loaded: every entity with its name, transform, parent and the components a game registers.

use std::collections::HashMap;
use std::path::Path;

use serde::de::DeserializeOwned;
use serde::Serialize;
use serde_json::{json, Map, Value};

use crate::{Entity, Name, Parent, Transform, World};

type Save = Box<dyn Fn(&World, Entity) -> Option<Value>>;
type Load = Box<dyn Fn(&mut World, Entity, &Value) -> Result<(), String>>;

/// The component types a level can hold, by the name they're saved under.
#[derive(Default)]
pub struct Registry {
    kinds: Vec<(String, Save, Load)>,
}

impl Registry {
    /// With Name and Transform already in.
    pub fn new() -> Registry {
        let mut r = Registry::default();
        r.add::<Name>("Name");
        r.add::<Transform>("Transform");
        r
    }

    pub fn add<T: Serialize + DeserializeOwned + 'static>(&mut self, name: &str) {
        self.kinds.push((
            name.to_string(),
            Box::new(|w, e| w.get::<T>(e).and_then(|v| serde_json::to_value(v).ok())),
            Box::new(|w, e, v| {
                let value: T = serde_json::from_value(v.clone()).map_err(|err| err.to_string())?;
                w.insert(e, value);
                Ok(())
            }),
        ));
    }

    /// The level as JSON: `{"format": 1, "entities": [{"id": 3, "parent": 1, "components": {...}}]}`.
    pub fn save(&self, world: &World) -> Value {
        let mut list = Vec::new();
        for e in world.entities() {
            let mut comps = Map::new();
            for (name, save, _) in &self.kinds {
                if let Some(v) = save(world, e) {
                    comps.insert(name.clone(), v);
                }
            }
            let mut item = json!({ "id": e.index, "components": comps });
            if let Some(Parent(p)) = world.get::<Parent>(e) {
                item["parent"] = json!(p.index);
            }
            list.push(item);
        }
        json!({ "format": 1, "entities": list })
    }

    /// Adds a saved level's entities to the world. Unknown components are kept out with a note, not an error.
    /// Returns the new entities by their saved ids, and the notes.
    pub fn load(&self, world: &mut World, level: &Value) -> Result<(HashMap<u64, Entity>, Vec<String>), String> {
        if level["format"].as_u64() != Some(1) {
            return Err("this isn't a Hearth level (format 1)".into());
        }
        let items = level["entities"].as_array().ok_or("the level has no entity list")?;
        let mut ids = HashMap::new();
        let mut notes = Vec::new();
        for it in items {
            let id = it["id"].as_u64().ok_or("an entity has no id")?;
            if ids.insert(id, world.spawn()).is_some() {
                return Err(format!("two entities have the id {id}"));
            }
        }
        for it in items {
            let e = ids[&it["id"].as_u64().unwrap_or_default()];
            for (name, value) in it["components"].as_object().into_iter().flatten() {
                match self.kinds.iter().find(|(n, _, _)| n == name) {
                    Some((_, _, load)) => load(world, e, value).map_err(|err| format!("entity {}: its {name} is broken: {err}", it["id"]))?,
                    None => notes.push(format!("entity {}: \"{name}\" isn't a component this game has, so it was left out", it["id"])),
                }
            }
        }
        for it in items {
            if let Some(p) = it["parent"].as_u64() {
                let child = ids[&it["id"].as_u64().unwrap_or_default()];
                let parent = *ids.get(&p).ok_or_else(|| format!("entity {}'s parent {p} isn't in the level", it["id"]))?;
                world.set_parent(child, Some(parent))?;
            }
        }
        world.update_transforms();
        Ok((ids, notes))
    }

    pub fn save_file(&self, world: &World, path: &Path) -> Result<(), String> {
        let text = serde_json::to_string_pretty(&self.save(world)).map_err(|e| e.to_string())?;
        hearth_save::write_atomic(path, text.as_bytes())
    }

    pub fn load_file(&self, world: &mut World, path: &Path) -> Result<(HashMap<u64, Entity>, Vec<String>), String> {
        let level = match hearth_save::load(path) {
            hearth_save::Loaded::Good(v) | hearth_save::Loaded::FromBackup(v) => v,
            hearth_save::Loaded::Missing => return Err(format!("there's no level at {}", path.display())),
            hearth_save::Loaded::Broken(why) => return Err(why),
        };
        self.load(world, &level)
    }
}
