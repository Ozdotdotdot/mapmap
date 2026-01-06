Waymap (Personal Fork)
======================

This repository is an active rewrite of MapMap that I use to power the projector that doubles as my living-room TV. The original MapMap project was archived by its creators about five years ago, so this fork is opinionated, experimental, and tailored to my own setup rather than a general-purpose release. 

## Why This Exists

I wanted to fill an entire wall with a mix of a standard “TV” region, shader-driven visuals, and reactive audio content. Existing projection-mapping tools on Linux fell short—none of them could simply capture and map an arbitrary screen or application window straight into a composition. That feature is now built into this fork and forms the foundation for everything else I plan to layer on top.

## Current Highlights

- **Wayland-native screen/application sharing.** Capture any monitor or window and map it without having to route through XWayland. Developed and tested on Arch Linux running COSMIC.
- **Designed for immersive projection setups.** The workflow assumes a projector-as-TV scenario and emphasizes mapping across an entire room instead of a single rectangular canvas.
- **Actively hacked-on foundation.** The codebase is mid-refactor, with large sections kept only because they power my daily use.

## Roadmap

1. Integrated audio visualizer that can be placed alongside the TV region.
2. Small shader library (GLSL) for ambient fills and transitions.
3. General polish once the above features feel solid enough for regular use.

There is no guarantee that any of these land in a way that is broadly useful, but they guide the direction of the work.

## Caveats & Expectations

- **Not production-ready.** Crashes, regressions, and half-implemented ideas are part of the experience. Treat every build as a prototype.
- **Support is limited to “whatever works on my machine.”** I test on Arch Linux under Wayland with COSMIC; other environments may require digging into the code.
- **Documentation is lagging.** This README is the canonical source of truth until the code stabilizes enough to justify deeper docs.

## Building & Running

The historical build instructions still apply. See `INSTALL.md` for dependency details, platform notes, and build commands. If you run into Wayland-specific snags, look at `WAYLAND_MIGRATION.md` for background on how the new capture pipeline is structured.

## Contributing

Pull requests are welcome if they align with the projector + shader + audio vision described above. If you have fixes for crashers, Wayland glitches, or general UX improvements, feel free to open an issue or draft PR so we can figure out how it fits.

## License

MapMap remains released under the [GNU GPL v3](LICENSE).
