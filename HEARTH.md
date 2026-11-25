# Hearth Engine

Hearth Engine is our own version of [O3DE](https://github.com/o3de/o3de), forked from the
**O3DE 26.10 release** (tag `2610.0`). Our work lives on the `hearth/main` branch.

Goals:

- Run really well on a modest laptop (Intel i3-N305, integrated Intel graphics, 8 GB RAM).
- Only the engine parts our games use; everything else stays out of the build.
- Hearth Engine tools: the Hub, a restyled editor, the Level Generator, full-screen playtests,
  and an agent that lets Claude build, run and test the game.

Build on the laptop:

    scripts/hearth/build-laptop.sh ~/O3DE/Projects/NoclipProtocol

## License

O3DE is dual-licensed under the Apache License 2.0 and the MIT License (see `LICENSE.txt`,
`LICENSE_APACHE2.TXT` and `LICENSE_MIT.TXT`); those notices stay in place. Hearth Engine's
changes are offered under the same terms. "Open 3D Engine" and "O3DE" are names of the
Open 3D Foundation; Hearth Engine is not affiliated with or endorsed by it.
