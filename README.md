# ApeEscapeRecomp

> _This recompilation is a **byproduct of developing
> [psxrecomp](https://github.com/mstan/psxrecomp)** — the games are the proving ground, the framework is the goal.
> **These are in-development previews, not finished ports — expect rough
> edges**, and depth will keep landing over months, not days. My time for any
> one title is limited, so I ask for your patience. Contributions are welcome —
> testing, issues, and PRs to the game or framework all help and will
> accelerate this game's polish. More on the why at:
> [Recomp + AI: 5 Months Later »](https://1379.tech/recomp-ai-5-months-later/)_

Ape Escape (USA, SCUS-94423) statically recompiled to a native PC executable
with [PSXRecomp](https://github.com/mstan/psxrecomp) — the same framework behind
[TombaRecomp](https://github.com/mstan/TombaRecomp) and
[MegaManX6Recomp](https://github.com/mstan/MegaManX6Recomp).

Contributions are welcome on both sides — to **psxrecomp** itself and to
refining specific games like Ape Escape. Known issues live in
[`ISSUES.md`](ISSUES.md).

## What This Is

This repository contains the game-specific configuration, seeds, tools, and
build glue for running Ape Escape on the PSXRecomp framework. The game's MIPS
code is machine-translated ("recompiled") ahead of time into native C, then
compiled into a real Windows program that runs the game's own logic on a
faithful simulation of the PS1 hardware (GPU, SPU, GTE, memory cards) plus the
real, recompiled PS1 BIOS — no high-level emulation shims.

It does **not** contain the Ape Escape disc image, the PS1 BIOS, generated game
code, or any decompiled game C. Those are produced locally from your own legally
obtained assets.

Important files:

- `game.toml`: runtime / recompiler / video / controller / widescreen config.
- `seeds/`: Ghidra-derived function starts and game-specific seed data.
- `tools/regen.ps1`: regenerates the recompiled C output.
- `tools/package_release.ps1`: builds the redistributable release zip.
- `psxrecomp-v4.pin`: framework commit this project is known-good against.
- `ISSUES.md`: game-specific issue log.
- `DISC.md`: source-disc identity and verification hashes.

## Status

**Playable preview — `v0.0.5-alpha`.** Ape Escape **boots from the PS1 BIOS and
plays** — through the intro, the title, and into gameplay, with dual-analog
controller input including **L3/R3 stick clicks** (added in v0.0.3), a
**controls fix** so the analog stick no longer spins the camera (v0.0.5),
working memory-card **save/load** (fixed in v0.0.2), and **no known crashes**. It has not yet been verified all the way to the end,
so treat it as a very playable preview rather than a certified full playthrough.

| Area | State |
|---|---|
| Boot (real PS1 BIOS) | ✅ Boots to intro / title / gameplay |
| Rendering | ✅ OpenGL (default) and Software backends |
| Controller | ✅ DualShock analog (auto-bound; the net/movement scheme is dual-stick) |
| Memory cards | ✅ Standard PS1 `.mcd` save/load |
| FMV / audio | ✅ MDEC video + XA/SPU audio (auto-skip FMV optional) |
| Widescreen 16:9 / 21:9 | ⚠️ Experimental (opt-in) — see below |
| Full playthrough | ⚠️ Not yet verified end-to-end |

### Experimental widescreen

An **experimental 16:9 / 21:9 mode** is available in the launcher (off by
default; the game ships authentic 4:3). It uses the stable GTE
projection-and-stretch path for a wider 3D field of view. The title sky and
ferris-wheel cabin regressions are fixed; the remaining very-wide draw-distance
limitation is tracked in [`ISSUES.md`](ISSUES.md). Regular 4:3 play is
byte-for-byte the original presentation and is unaffected.

## Playing

1. Run `ApeEscapeRecomp.exe`. A launcher window opens.
2. Set your PlayStation BIOS (a legally obtained `SCPH1001.BIN`, 512 KB).
3. Set the game disc (a legally obtained Ape Escape (USA, SCUS-94423) image —
   `.cue`+`.bin`, pick the `.cue`). Do **not** convert to a 2048-byte "cooked"
   `.iso`; that discards the XA sectors used for FMV/audio.
4. Adjust options if you like (renderer, supersampling, experimental widescreen),
   then press **Launch**.

Ape Escape is a dual-analog title — the right stick swings the catch net — so an
analog controller is strongly recommended. Any plugged pad is auto-bound and
presented to the game as a DualShock; a keyboard folds onto the analog stick.
The selected BIOS/disc paths and options are saved next to the exe.

## Development Rules

- Use the real recompiled BIOS and real hardware simulation in PSXRecomp.
- No HLE BIOS shims, no stubs, no fake events, no hand-edited generated files.
- Framework changes go in `mstan/psxrecomp`, not here.
- Game binaries, generated code, memory cards, Ghidra databases, and build
  outputs stay local.
- See `CLAUDE.md` for project-specific rules.

## License

PolyForm Noncommercial 1.0.0. See `LICENSE`.

Ape Escape is copyright Sony Computer Entertainment / SIE. This repository
contains none of the game's original binaries or assets. Release packages
contain no game assets, no disc data, and no BIOS image — those are always read
from files you supply. The release executable contains a statically recompiled
(machine-translated) build of the game's code, the same distribution model used
by other static recompilation projects such as N64: Recompiled.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
