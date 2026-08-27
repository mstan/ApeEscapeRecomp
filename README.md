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

Known issues live in [`ISSUES.md`](ISSUES.md).

## What This Is

This repository contains the game-specific configuration, seeds, tools, and
build glue for running Ape Escape on the PSXRecomp framework. The game's MIPS
code is machine-translated ("recompiled") ahead of time into native C, then
compiled into a real Windows program that runs the game's own logic on a
faithful simulation of the PS1 hardware (GPU, SPU, GTE, memory cards) plus the
real, recompiled PS1 BIOS — no high-level emulation shims.

It does **not** contain the Ape Escape disc image, a retail PS1 BIOS, generated
game code, or any decompiled game C. Release builds include the MIT-licensed
OpenBIOS from PCSX-Redux; game data and an optional retail BIOS come from your
own legally obtained assets.

Important files:

- `game.toml`: runtime / recompiler / video / controller / widescreen config.
- `mods/preloaded/`: built-in, default-off mod packages shown by the launcher.
- `seeds/`: Ghidra-derived function starts and game-specific seed data.
- `tools/regen.ps1`: regenerates the recompiled C output.
- `tools/package_release.ps1`: builds the redistributable release zip.
- `psxrecomp-v4.pin`: framework commit this project is known-good against.
- `ISSUES.md`: game-specific issue log.
- `DISC.md`: source-disc identity and verification hashes.

## Status

**Playable preview — `v0.2.1`.** Ape Escape **boots from the PS1 BIOS and
plays** — through the intro, the title, and into gameplay, with dual-analog
controller input including **L3/R3 stick clicks** (added in v0.0.3), a
**controls fix** so the analog stick no longer spins the camera (v0.0.5),
and working memory-card **save/load** (fixed in v0.0.2). It has not yet been verified all the way to the end,
so treat it as a very playable preview rather than a certified full playthrough.

| Area | State |
|---|---|
| Boot (real PS1 BIOS) | ✅ Boots to intro / title / gameplay |
| Rendering | ✅ OpenGL (default) and Software backends |
| Controller | ✅ DualShock analog (auto-bound; the net/movement scheme is dual-stick) |
| Memory cards | ✅ Standard PS1 `.mcd` save/load |
| Save states / rewind | ✅ Launcher-backed hotkeys and runtime state capture |
| FMV / audio | ✅ MDEC video + XA/SPU audio (auto-skip FMV optional) |
| Mods | ✅ Built-in catalog with Ape-specific and framework-owned enhancements |
| Widescreen 16:9 / 21:9 / Adaptive | ✅ Opt-in 16:9, 21:9, and live-window adaptive modes |
| Temporal frame blending | ✅ Opt-in display / 120 / 144 / 165 presents-per-second modes |
| Full playthrough | ⚠️ Not yet verified end-to-end |

### Built-in mods

The launcher's **Mods** page includes four Ape-specific bundled enhancements:

- **Ape Escape Widescreen** moves the existing game-specific enhancement out
  of generic Video settings. Its picker offers **16:9**, **21:9**, and
  **Adaptive** (live window aspect from 4:3 through 21:9).
- **Ape Escape Frame Smoothing** crossfades completed game frames at the
  display refresh or a fixed **120**, **144**, or **165 presents/s**. It leaves
  the executable, simulation, timers, and audio at stock cadence. This is
  temporal blending, not motion-vector frame generation, so it cannot create
  true in-between object positions. All OpenGL presentation remains on the
  renderer's original thread and context.
- **Skip FMVs** ends movies through the game's normal completion path.
- **Quick Gadget Select**, contributed by mthsk, recreates the later Ape
  Escape quick gadget switching flow for the face-button gadget menu.

The widescreen mod uses Ape Escape's stable GTE projection-and-stretch path
for a wider 3D field of view. HUD/UI proportion correction is enabled for its
front ordering-table layer; remaining draw-distance and scene-culling limits
are tracked in [`ISSUES.md`](ISSUES.md). With the mod disabled, 4:3
presentation is unchanged.

The launcher's Video page exposes **Supersampling (1x-4x)**,
**Antialiasing**, and **Texture filtering**. On OpenGL, supersampling is true
ordered-grid SSAA: geometry and shading are rasterized at a higher internal
resolution and resolved to the window, while native PSX VRAM remains the
authoritative game-visible buffer. Antialiasing controls the linear SSAA resolve
and final presentation filtering; it is not RT64-style MSAA. Try **2x** first on
a discrete GPU, keep texture filtering at **Nearest** for the authored PSX look,
and return to 1x if a demanding scene misses full speed.

## Playing

1. Run `ApeEscapeRecomp.exe`. A launcher window opens.
2. OpenBIOS is selected automatically. Optionally select your legally obtained
   `SCPH1001.BIN` in the BIOS row.
3. Set the game disc (a legally obtained Ape Escape (USA, SCUS-94423) image —
   `.cue`+`.bin`, pick the `.cue`). Do **not** convert to a 2048-byte "cooked"
   `.iso`; that discards the XA sectors used for FMV/audio.
4. Adjust display options and select any features on the **Mods** page,
   then press **Launch**.

Ape Escape is a dual-analog title — the right stick swings the catch net — so an
analog controller is strongly recommended. Any plugged pad is auto-bound and
presented to the game as a DualShock; a keyboard folds onto the analog stick.
Save states and rewind are exposed through launcher hotkeys; by default F7 opens
the save-state menu and F8 rewinds.
The disc path, optional retail BIOS choice, and options are saved next to the
exe. Clear the BIOS row to return to OpenBIOS.

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
include PCSX-Redux OpenBIOS under the MIT notice in
`bios/OpenBIOS.LICENSE`; they contain no retail BIOS, game assets, or disc data.
The release executable contains a statically recompiled (machine-translated)
build of the game's code.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
