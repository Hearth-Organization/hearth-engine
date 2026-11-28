//! Audio for Hearth World entities: Kira plays the sounds (3D, so they come from where things are), and Hearth's
//! own acoustics (the Steam Audio idea, written in Rust) make them fit the place:
//!
//! - **Behind walls, muffled**: physics rays from the listener to each sound count the walls in between; each
//!   wall cuts the high sounds and the loudness, so a monster behind a door sounds behind a door.
//! - **Echoes that fit the room**: rays out from the listener measure the space; a big empty hall echoes long,
//!   a small room barely, outdoors not at all.

pub mod acoustics;

use std::collections::HashMap;
use std::sync::Arc;

use glam::{Quat, Vec3};
use hearth_physics::Physics;
use hearth_world::{Entity, GlobalTransform, World};
use kira::backend::Backend;
use kira::effect::filter::{FilterBuilder, FilterHandle};
use kira::effect::reverb::{ReverbBuilder, ReverbHandle};
use kira::listener::ListenerHandle;
use kira::sound::static_sound::{StaticSoundData, StaticSoundHandle};
use kira::track::{SpatialTrackBuilder, SpatialTrackHandle, TrackBuilder, TrackHandle};
use kira::{AudioManager, AudioManagerSettings, Tween};
use serde::{Deserialize, Serialize};

/// The entity that hears (the player's head or the camera).
#[derive(Clone, Copy, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Listener;

/// A sound coming from an entity.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Emitter {
    /// A name in the sound library.
    pub sound: String,
    /// Decibels (0 = as recorded, -6 = half as loud).
    pub volume: f32,
    pub looping: bool,
    /// Heard out to this many metres.
    pub range: f32,
}

struct Playing {
    track: SpatialTrackHandle,
    filter: FilterHandle,
    sound: StaticSoundHandle,
    walls: u32,
}

pub struct Audio<B: Backend> {
    manager: AudioManager<B>,
    listener: ListenerHandle,
    library: HashMap<String, StaticSoundData>,
    playing: HashMap<Entity, Playing>,
    room: TrackHandle,
    reverb: ReverbHandle,
    /// How walls muffle and echoes work (a game can change these).
    pub acoustics: acoustics::Settings,
    since_room: f32,
}

fn smooth() -> Tween {
    Tween { duration: std::time::Duration::from_millis(120), ..Tween::default() }
}

impl<B: Backend> Audio<B> {
    pub fn new(settings: AudioManagerSettings<B>) -> Result<Audio<B>, String> {
        let mut manager = AudioManager::<B>::new(settings).map_err(|e| format!("the sound device wouldn't start: {e:?}"))?;
        let listener = manager.add_listener(Vec3::ZERO, Quat::IDENTITY).map_err(|e| format!("{e:?}"))?;
        // The room's echo: everything is sent through it, more or less depending on the space.
        let mut room_builder = TrackBuilder::new();
        let reverb = room_builder.add_effect(ReverbBuilder::new().mix(1.0f32).feedback(0.0).damping(0.6));
        let room = manager.add_sub_track(room_builder).map_err(|e| format!("{e:?}"))?;
        Ok(Audio { manager, listener, library: HashMap::new(), playing: HashMap::new(), room, reverb, acoustics: acoustics::Settings::default(), since_room: 1.0 })
    }

    /// Adds a sound file (wav, ogg, mp3, flac) to the library under a name.
    pub fn load(&mut self, name: &str, path: &std::path::Path) -> Result<(), String> {
        let data = StaticSoundData::from_file(path).map_err(|e| format!("can't load the sound {}: {e}", path.display()))?;
        self.library.insert(name.to_string(), data);
        Ok(())
    }

    /// Adds a sound made in code (stereo frames at a sample rate).
    pub fn add_frames(&mut self, name: &str, sample_rate: u32, frames: Vec<kira::Frame>) {
        self.library.insert(name.to_string(), StaticSoundData { sample_rate, frames: Arc::from(frames), settings: Default::default(), slice: None });
    }

    pub fn has(&self, name: &str) -> bool {
        self.library.contains_key(name)
    }

    /// Brings the sound in step with the world: new emitters start, removed ones stop, everything moves with its
    /// entity, and the acoustics follow (walls between, the room's size). Needs `update_transforms` first.
    pub fn update(&mut self, w: &World, physics: Option<&Physics>, dt: f32) -> Vec<String> {
        let mut problems = Vec::new();
        let ear = w.query::<Listener>().next().and_then(|(e, _)| w.get::<GlobalTransform>(e).map(|g| (e, g.0)));
        let ear_pos = ear.map(|(_, m)| m.transform_point3(Vec3::ZERO)).unwrap_or(Vec3::ZERO);
        if let Some((_, m)) = ear {
            let (_, rotation, _) = m.to_scale_rotation_translation();
            self.listener.set_position(ear_pos, smooth());
            self.listener.set_orientation(rotation, smooth());
        }
        // Gone or no longer emitting: stop.
        let gone: Vec<Entity> = self.playing.keys().copied().filter(|&e| !w.is_alive(e) || !w.has::<Emitter>(e)).collect();
        for e in gone {
            if let Some(mut p) = self.playing.remove(&e) {
                p.sound.stop(smooth());
            }
        }
        for (e, em) in w.query::<Emitter>() {
            let pos = w.get::<GlobalTransform>(e).map(|g| g.0.transform_point3(Vec3::ZERO)).unwrap_or(Vec3::ZERO);
            if !self.playing.contains_key(&e) {
                let Some(data) = self.library.get(&em.sound) else {
                    problems.push(format!("{e:?}: there's no sound called \"{}\" in the library", em.sound));
                    continue;
                };
                let mut builder = SpatialTrackBuilder::new().distances((1.0, em.range.max(1.1))).volume(em.volume).with_send(&self.room, -12.0f32);
                let filter = builder.add_effect(FilterBuilder::new().cutoff(20_000.0));
                let mut track = match self.manager.add_spatial_sub_track(&self.listener, pos, builder) {
                    Ok(t) => t,
                    Err(e) => {
                        problems.push(format!("too many sounds at once: {e:?}"));
                        continue;
                    }
                };
                let mut data = data.clone();
                if em.looping {
                    data = data.loop_region(..);
                }
                match track.play(data) {
                    Ok(sound) => {
                        self.playing.insert(e, Playing { track, filter, sound, walls: 0 });
                    }
                    Err(err) => problems.push(format!("{e:?}: the sound wouldn't play: {err:?}")),
                }
                continue;
            }
            let p = self.playing.get_mut(&e).expect("just checked");
            p.track.set_position(pos, smooth());
            if let Some(ph) = physics {
                let walls = acoustics::walls_between(ph, ear_pos, pos, ear.map(|(e, _)| e), Some(e));
                if walls != p.walls {
                    p.walls = walls;
                    let (cutoff, db) = self.acoustics.muffle(walls);
                    p.filter.set_cutoff(cutoff as f64, smooth());
                    p.track.set_volume(em.volume + db, smooth());
                }
            }
        }
        // The room's echo, a few times a second.
        self.since_room += dt;
        if let (Some(ph), true) = (physics, self.since_room >= 0.25) {
            self.since_room = 0.0;
            let echo = self.acoustics.room(|dir| ph.ray(ear_pos, dir, 60.0, ear.map(|(e, _)| e)).map(|h| h.1));
            self.reverb.set_feedback(echo.feedback as f64, Tween { duration: std::time::Duration::from_millis(400), ..Tween::default() });
            self.room.set_volume(echo.send_db, smooth());
        }
        problems
    }

    pub fn playing(&self) -> usize {
        self.playing.len()
    }

    /// Walls between the listener and an entity's sound, as last worked out.
    pub fn walls(&self, e: Entity) -> Option<u32> {
        self.playing.get(&e).map(|p| p.walls)
    }
}
