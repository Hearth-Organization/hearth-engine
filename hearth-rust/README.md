# Hearth Engine's Rust parts

The owner, Nov 28, 2026: keep O3DE's C++ core and make Hearth's own parts Rust, in one window (no tool opens a window
of its own). Built and tested on GitHub only (`.github/workflows/hearth-rust.yml`).

- `hearth-link`: the connection between the Rust parts and the editor (open levels, place and move things, Play,
  the selection, open a tool as a tab, console, screenshots), plus the Hearth Engine agent's API. It talks to the
  editor's bridge (`Editor/Scripts/claude_bridge.py` in the game: 127.0.0.1, a per-session token, the editor's main
  thread) and never pastes values into code. Also a command, `hearth-link`, for Claude and scripts:
  `hearth-link --project ~/O3DE/Projects/NoclipProtocol level`.

- `hearth-helper`: the Hearth Engine helper in Rust, a port of the game's `Tools/HearthAgent/hearth_agent.py` (1.10):
  the same 46 operations, request files and Hub API (127.0.0.1:47830), for every game in the projects folder at once
  (each request names its game; X-Hearth-Project on the API). It shares the Python helper's lock, so only one of the
  two runs. The Python helper stays the one that runs until the owner has tried this one.
- `hearth-script`: Hearth Script, the small language for what happens in a game (doors, keys, monsters, rules), for
  people who've never written code. The whole language is in `hearth-script/GUIDE.md`. Mistakes are explained in plain
  words with the line (and "did you mean"), a loop that never ends is stopped instead of freezing the game, a script
  can only call what the game gives it, and a reload keeps `keep let` values and never replaces the last good version
  with one that has a mistake. The Simple editor's When / Then cards are saved as Hearth Script (`rules.rs`). A
  command, `hearth-script door.hs picked_up "Gym key"`, runs a script; `--check` only looks for mistakes. Not yet
  wired into the editor or the game.

- `hearth-detail`: Nanite-style detail, the builder half. A model (.glb from Hearth Forge, or .obj) is cut into pieces
  of up to 128 triangles; groups of neighbouring pieces get simpler versions, level after level, with their outside
  edges kept still, and each piece records its error and its parent's. The game draws, piece by piece, the simplest
  version whose error is too small to see: full detail up close, far fewer triangles far away, never a crack (the
  tests check every view of a closed ball and an open, seamed sheet). `hearth-detail model.glb` writes
  `model.hdetail`. The drawing half (picking pieces on the graphics card each frame, in Atom) comes next.
- `hearth-rts`: the real-time strategy kit. Flow fields (hundreds of units share one path, mud and water avoided,
  no corner cutting), units and buildings, orders (move, attack-move, attack, hold, gather, build; queued with
  shift), groups that spread out at the goal, fighting, gathering, training with costs and supply, buildings that
  block paths, fog of war per team, drag-select, click-select and control groups, and events for sounds and Hearth
  Script. The same orders always give the same game (for multiplayer that sends only orders).
- `hearth-kits`: the other genres' parts. Platformer movement (named feel numbers, coyote time, jump buffer, short
  hops), dialogue written as plain text with choices and remembered flags, quests that count game events and unlock
  the next, inventory stacks and crafting (with workbenches), turn-based tactics (turn order by speed, where a unit
  can walk, who it can hit), and tower defense (waves, paths that change when towers go up, never fully blocked).

- `hearth-ffi`: the bridge the engine's HearthRust gem calls (C interface to Hearth Script and Hearth Detail).
- `hearth-save`: safe saving for games and tools: atomic writes, a backup, never defaults over a broken file,
  merge on write, save slots with summaries.
- `hearth-levelgen`: the Level Generator core: endless worlds built chunk by chunk from a seed, every chunk
  connected to its neighbours (at least two doorways a side), no wall built twice, per-cell choices and smooth zones.
- `hearth-pack`: Package and Share (Hearth OS AppImage with SHA256SUMS, Steam depot folder, test archive), a port
  of the game's finish_package.py with the game's own files given as extras; `hearth-pack verify` checks a download.
- `hearth-import`: import checks for models in plain words (wrong units, lying on its side, far from the origin,
  inside out, no uvs, holes, tangled edges, too many triangles).
- `hearth-logs`: crash and log reports: errors grouped, the crash and what came just before, fixes for the common
  problems.
- `hearth-playtest`: playtest reports from JSON-lines recordings: deaths and killers, stuck spots, slow places,
  time per level, quits, and the deadliest places across many players.

The Hearth list is done (the Voice Studio link, co-op lobby, Hub start page and build commands came over with
`hearth-helper`). Next: the rest of the engine, piece by piece (the owner, Nov 28: "start rewriting everything else
in hearth engine as well in rust"): the project and gem tools first, then the asset pipeline, gameplay systems,
physics and audio, and the renderer last.
