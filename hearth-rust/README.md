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

Next, in this order: wiring Hearth Script into the game, the Level Generator's core, packaging and Share, import
checks, safe saving.
