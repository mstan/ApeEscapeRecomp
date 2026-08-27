# Ape Escape Recompiled - Unreleased

- Removes the bundled Frame Rate / temporal frame-blending mod while issue #3
  is unresolved. Stock gameplay remains unchanged; the removed package was
  default-off but could freeze above 60 Hz and did not provide true in-between
  motion.

---

# Ape Escape Recompiled - v0.2.0

This minor release adds Quick Gadget Select as a bundled gameplay mod, formally
ships save states and rewind, and refreshes the runtime/UI pins to the latest
release-tested PSXRecomp stack.

## New features

- **Quick Gadget Select**, contributed by mthsk, is now bundled on the Mods
  page. Press the equipped face button again to open the native-icon gadget row;
  right-stick use or another face-button press commits the selection.
- **Save states and rewind** are now part of the formal release surface. By
  default, F7 opens the save-state menu and F8 rewinds.

## Runtime and packaging

- Updates psxrecomp to include the latest CD, SIO/memory-card, dispatch, and
  save-state boundary fixes, plus the display-size plugin API needed by Quick
  Gadget Select.
- Updates recomp-ui to the latest launcher fixes, including small-display
  window fitting and current mod-page behavior.
- The Windows package now ships Ape's three title-specific mod packages beside
  the current framework-owned PSX mod catalog.

---

# Ape Escape Recompiled — v0.1.1

This patch improves the temporal frame-blending mod and trims the
launcher package to PlayStation-only assets.

## Frame blending

- Replaces the full-frame linear crossfade with a motion-adaptive clarity
  blend. Large pixel changes switch cleanly instead of showing two translucent
  poses, substantially reducing trails around moving characters and scenery.
- Follows the display refresh by default while retaining fixed 60, 120, 144,
  and 165 presentation-rate choices for testing.
- Keeps guest simulation, physics, timers, animation, and audio at their stock
  cadence. This remains temporal blending rather than motion-vector frame
  generation.

## Packaging

- Updates recomp-ui's asset staging so the Ape Escape package contains common
  launcher chrome and PlayStation art only, without controller or cartridge
  assets belonging to other consoles.

---

# Ape Escape Recompiled — v0.1.0

This release makes Ape Escape substantially smoother to control and present,
and moves its optional enhancements into the launcher's Mods catalog.

## Controls and presentation

- **Better joystick response.** Both sticks now use a radial response tuned
  around Ape Escape's own internal deadzone. Movement speed scales linearly
  from the edge of the host deadzone to full travel, cardinal and diagonal
  inputs reach the same speed, and gadget aiming preserves its direction
  without diagonal notching.
- **Frame interpolation.** The default-off Interpolated Frame Rate mod offers
  60, 120, 144, 165, and uncapped presentation while leaving game logic,
  physics, timers, and audio at the original cadence.
- **Better widescreen culling.** World models and terrain cells stay active
  across the wider frustum, substantially reducing side-of-screen pop-in.
  Grouped front-layer HUD correction also keeps more UI at the intended
  proportions.

## Mods and runtime

- Widescreen, interpolation, and Skip FMVs are now built-in, default-off
  packages on the launcher's **Mods** page.
- Widescreen offers 16:9, 21:9, and Adaptive modes; Adaptive follows the live
  window aspect from 4:3 through 21:9.
- OpenBIOS is included and selected by default. You can still choose your own
  legally obtained retail BIOS.
- The launcher and runtime include the latest mod-owned settings and renderer
  UI fixes.

## Before playing

Supply your own legally obtained Ape Escape (USA, SCUS-94423) disc image. It is
not included. This remains an in-development preview, so keep your saves backed
up and report any game-specific regressions.

---

# Ape Escape Recompiled — v0.0.6-alpha

Ape Escape boots from the real PlayStation BIOS and **plays** as a native
Windows program with no emulator behind it, on the
[PSXRecomp](https://github.com/mstan/psxrecomp) framework — the same pipeline
behind TombaRecomp and MegaManX6Recomp.

## ✨ New in v0.0.6 — brand-new launcher + widescreen fixes

- **A completely redesigned launcher.** Ape Escape now uses the shared
  [recomp-ui](https://github.com/mstan/recomp-ui) launcher — a PlayStation-themed
  front-end that replaces the old one:
  - **Real disc verification** — reads your disc's serial, region, and ISO header
    and shows a clear verified / warn / wrong-disc verdict.
  - **Memory cards** — per-slot enable, Browse an existing card or create a fresh
    blank one, with a live block-usage grid reading your actual saves.
  - **Deep display options** — aspect ratio (4:3 / 16:9 / 21:9), renderer,
    supersampling, antialiasing (Off / 2× / 4× / 8×), screen model, frame
    interpolation, and more.
  - **Controllers** — analog / digital / hybrid pad modes and full keyboard
    rebinding (all 24 inputs, including the analog-stick directions).
- **Widescreen validation continues.** UI proportion, character projection,
  and scene-culling behavior are being image-validated as the renderer evolves.

## 🕹️ New in v0.0.5 — controls fix (phantom camera rotation)

- **The camera no longer spins on its own, and the analog stick no longer
  rotates it.** On a controller, pushing the left stick to move — or even a
  slight stick centre-drift at rest — was also being read as D-pad left/right,
  which Ape Escape uses to rotate the camera. So the camera drifted constantly
  and swung whenever you moved. The left stick and the D-pad are now
  independent, exactly like a real DualShock: the **left stick moves** (analog
  only) and the **D-pad rotates the camera**. If your camera was rotating with
  no input, this release is the fix. Settings and saves carry over. Thanks to
  VGEsoterica for the report.

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

## ✨ Widescreen (16:9 / 21:9)

Off by default — the game ships authentic 4:3. Turn it on in the launcher
(**Widescreen**). It renders a genuinely wider field of view:

- The 3D world fills the wider frame through Ape's stable
  projection-and-stretch path.
- The title's curved sky mesh fills the wide frame without affecting attract
  demo geometry.
- Ferris-wheel cabins remain visible throughout the amusement-park shot.
- 21:9 is also available (a fully-3D title has no authored-parallax ceiling).

Known rough edge on the widescreen path (4:3 is unaffected): very-wide views can
still expose the game's original distance pop-in. See `ISSUES.md`.

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
