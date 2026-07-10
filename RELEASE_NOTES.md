# Ape Escape Recompiled — v0.0.4-alpha

Ape Escape boots from the real PlayStation BIOS and **plays** as a native
Windows program with no emulator behind it, on the
[PSXRecomp](https://github.com/mstan/psxrecomp) framework — the same pipeline
behind TombaRecomp and MegaManX6Recomp.

## 🛠️ New in v0.0.4 — critical boot fix

- **The v0.0.2 and v0.0.3 zips could not boot on user machines — fixed.**
  Those builds relied on a reference copy of the game's boot executable that
  only exists in a development checkout (it is game data, so it is never
  shipped in the zip). Without it the runtime's text-integrity guard never
  armed and the boot died silently just after the BIOS handed control to the
  game. The runtime now extracts that reference directly from **your disc
  image** — the same bytes the BIOS boots — so it works on every install.
  If you downloaded v0.0.2 or v0.0.3 and it closed itself moments after
  launch, this release is the fix; settings and saves carry over.

## 🆕 New in v0.0.3

- **L3 / R3 (stick clicks) now work.** Ape Escape uses the stick-click buttons
  for core moves, and they previously could not be produced by any input
  device. They are now first-class across the whole input stack:
  - **Controller:** clicking the left/right stick sends L3/R3 out of the box
    (`l3 = leftstick`, `r3 = rightstick` in `input.ini`, remappable).
  - **Keyboard:** bound to **T** / **Y** by default (continuing the
    Q/W/E/R shoulder row), rebindable on the launcher's Controls page.
  - Existing installs pick the new defaults up automatically unless you had
    explicitly set `l3`/`r3` in your own `input.ini`/`keybinds.ini`.

## 🆕 New in v0.0.2

- **Memory-card save / load now works.** The card screen completes reliably:
  progress saves and loads back on standard PS1 `.mcd` images. This closes the
  one gap called out in v0.0.1 (issue #4) — a framework-level fix to how a
  cooperative thread-switch is deferred across interrupt delivery, so the card
  read/write is only resumed at a boundary where its CPU state is coherent.
  Validated against MegaManX6 and Tomba (1) with no regression.

## ✅ What works

- **Boots and plays.** PS1 BIOS → disc detect → intro → title → gameplay, with
  **no known crashes**.
- **Dual-analog controller.** Ape Escape is built around the DualShock's two
  sticks (the right stick swings the net). Any plugged pad is auto-bound and
  presented to the game as a DualShock, including L3/R3 stick clicks; a
  keyboard folds onto the analog stick.
- **Memory-card save / load.** Standard PS1 `.mcd` images, emulator-compatible.
- **FMV + audio.** MDEC video and XA/SPU audio play; FMVs can be auto-skipped.
- **OpenGL renderer by default**, with a Software renderer selectable in the
  launcher. Optional supersampling + anti-aliasing.
- **Instant-boot (HLE).** Skips the BIOS boot animation and drops you into the
  game; the real recompiled BIOS stays linked for everything else.

## ✨ Experimental: widescreen (16:9 / 21:9)

Off by default — the game ships authentic 4:3. Turn it on in the launcher
(**Widescreen — EXPERIMENTAL**). It renders a genuinely wider field of view:

- The 3D world fills the wider frame (the render funnel and object screen-culls
  are widened so geometry isn't clipped at the old 4:3 edge).
- The HUD (item ring, ammo, radar) is re-anchored to the true wide corners.
- A native-wide GL compositor optimization keeps it at a locked 60fps.
- 21:9 is also available (a fully-3D title has no authored-parallax ceiling).

Known rough edges on the widescreen path (4:3 is unaffected): the title/menu sky
"dome" doesn't reach the far corners on a few screens, and some objects still
pop in by distance or at the old edge. See `ISSUES.md`.

## Notes

- This package includes **no game assets, no disc data, and no BIOS** — you
  supply your own legally obtained Ape Escape (USA) disc image and `SCPH1001.BIN`
  on first launch. The executable contains a statically recompiled
  (machine-translated) build of the game's code, the same distribution model as
  N64: Recompiled.
- Disc formats: `.cue`+`.bin` (pick the `.cue`) or `.bin`. Do **not** convert to
  a 2048-byte "cooked" `.iso` — it discards the XA sectors used for FMV/audio.
- This is a very early preview; a full playthrough has not been verified.

PolyForm Noncommercial 1.0.0. Ape Escape is copyright Sony Computer
Entertainment / SIE.
