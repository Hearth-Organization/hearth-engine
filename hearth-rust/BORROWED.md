# What Hearth Engine borrows from open-source engines

The owner, Nov 28, 2026: "Anything we can rip / steal and build into Hearth engine is worth a shot so do it", then
"Add all that stuff to Hearth engine". Open-source code only, under licences that allow it, with their licence
notices kept next to the code (ideas only from closed engines).

| # | What | From (licence) | Where it goes | Status |
|---|------|----------------|---------------|--------|
| 1 | The graphics card picks each piece's detail (Nanite-style, GPU-driven) | Bevy's meshlet renderer (MIT/Apache-2.0) | Detail Mesh, Atom | planned |
| 2 | Sound muffled behind walls, echoes in halls | Steam Audio (Apache-2.0) | an audio gem | planned |
| 3 | Edit the game while it runs; see the running game's things and their values live | Godot (MIT) | editor + game | planned |
| 4 | Jolt physics (fast, used by Godot 4) | Jolt Physics (MIT), Rust bindings | physics, the rewrite | planned |
| 5 | Levels from a few example tiles (Wave Function Collapse) | WFC (MIT) | hearth-levelgen | building |
| 6 | Rollback netcode for smooth co-op | GGRS (MIT/Apache-2.0) | co-op, the RTS kit | planned |
| 7 | Game code in Rust that reloads while running; an animation state editor | Fyrox (MIT) | Hearth Script / Rust game code, animation | planned |
| 8 | Linked prefabs: change one, every copy follows, copies keep their own tweaks | Stride (MIT) | editor | planned |
| 9 | Build and test levels inside Blender | Armory3D (zlib) | a Blender add-on | planned |
| 10 | 2D tile map editor | Godot (MIT) | editor, 2D games | planned |
| 11 | Frame profiler: what makes a frame slow | Tracy (BSD-3) | engine + editor | planned |
| 12 | Clean ray-traced and baked lighting | Open Image Denoise (Apache-2.0) | Atom | planned |
| 13 | The Rust editor's tool panels; the audio rewrite | egui, Kira (MIT/Apache-2.0) | the rewrite | planned |
| 14 | Instant reloads, a simple node tree, an animate-anything timeline, a light laptop mode | Godot (MIT), Bevy | editor + engine | planned |
| 15 | Godot as a second engine inside Hearth Engine (in a game tab) | Godot (MIT) | game tabs | planned |
| 16 | ECS and scenes for the rewrite; Godot's 2D | Bevy, Godot | the rewrite | planned |
