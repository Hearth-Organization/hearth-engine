# Hearth Engine's Rust parts

The owner, Nov 28, 2026: keep O3DE's C++ core and make Hearth's own parts Rust, in one window (no tool opens a window
of its own). Built and tested on GitHub only (`.github/workflows/hearth-rust.yml`).

- `hearth-link`: the connection between the Rust parts and the editor (open levels, place and move things, Play,
  the selection, open a tool as a tab, console, screenshots), plus the Hearth Engine agent's API. It talks to the
  editor's bridge (`Editor/Scripts/claude_bridge.py` in the game: 127.0.0.1, a per-session token, the editor's main
  thread) and never pastes values into code. Also a command, `hearth-link`, for Claude and scripts:
  `hearth-link --project ~/O3DE/Projects/NoclipProtocol level`.

Next, in this order: the Hearth Engine helper (the agent, now Python), Hearth Script, the Level Generator's core,
packaging and Share, import checks, safe saving.
