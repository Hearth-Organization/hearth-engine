# What's new in Hearth Engine

## 1.1 (in progress)

The whole engine in Hearth's own look, built from the approved mockups:

- Hearth colours, the Geist font, rounded cards and pill buttons in every window and tool.
- The Hearth flame in place of the O3DE logo.
- New splash screen with real startup progress, and a new About box.
- One window: Voice Studio, Animation, Level Generator and every other Hearth tool open as tabs beside the 3D view (World), never as windows of their own unless you want one: drag a tab off the row (or right-click it › Open in its own window) and it becomes its own window; Put back in the tabs returns it. Play always goes back to World.
- One window, many games: a row of game tabs on top. + opens another game in this window; each game runs in its own engine and shows inside its tab, so switching is instant. On computers under 12 GB the games you aren't looking at pause after half a minute, and when memory runs short the one used longest ago is saved and closed (its tab says so; a click reopens it).

## 1.0

- First Hearth Engine .deb: the engine SDK built on GitHub, `hearth-engine`, `hearth-engine-editor` and
  `hearth-engine-assets` commands, and a log of every run in `~/O3DE/HearthEngine/logs`.

## The HearthRust gem (Nov 28, 2026)

Hearth Engine's Rust parts in the game, built by cargo as part of the engine (`Gems/HearthRust`, Rust in
`hearth-rust/hearth-ffi`):

- **Hearth Script** component: runs a `.hs` file on an entity (`position()`, `move_to(x, y, z)`, `time()`,
  `send(name, ...)` to the game), fires `start` and `tick(dt)`, reloads within a second of the file being saved
  (the last good version keeps running on a mistake). Script Canvas and Lua: `HearthScriptRequestBus.Fire` /
  `Reload`, and `HearthScriptNotificationBus` (`OnScriptSent`, `OnScriptMistake`, `OnScriptSaid`).
- **Detail Mesh** component: Nanite-style detail for a `.glb`, `.obj` or `.hdetail` model with a standard material.
  The pieces to draw are picked from the camera ten times a second; full detail up close, far fewer triangles far
  away, no cracks. Shows in game mode (Play) for now.
