# ApeEscapeRecomp — Issues

Current state (v0.0.2-alpha): Ape Escape **boots from the PS1 BIOS and plays** as
a native Windows program — through the intro, title, and gameplay, with dual-
analog controller input. It has not yet been verified all the way to the end, so
treat it as a very playable preview rather than a certified full playthrough.
**Memory-card save/load now works** (issue #4 fixed in v0.0.2) — progress saves
and loads back on standard `.mcd` images. **Phantom camera rotation is fixed**
(v0.0.5): the analog stick was doubling as the D-pad in analog mode, so stick
movement and centre-drift spun the camera; the stick and D-pad are now
independent as on real hardware.

An **experimental 16:9 / 21:9 widescreen** mode is available in the launcher
(off by default). It renders a genuinely wider field of view — the 3D world
fills the frame and the HUD sits at the true wide corners — with a per-game
compositor perf fix so it holds 60fps. Widescreen is **experimental and has
known visual bugs** — issues #1–#3 below are the known rough edges in that
mode. Regular **4:3 is the accurate default** and is unaffected.

---

## #1 — Widescreen: title/menu sky "dome" doesn't reach the wide corners — OPEN

In widescreen, the **title and some menu screens** draw their sky as a
GTE-projected 3D dome mesh authored to fill a 4:3 frame. Its curved edge doesn't
reach the corners of the wider frame, so those corners show black. The 3D world,
gameplay skies, and cutscene skies all fill correctly — this is specific to the
finite sky-dome billboards on a few screens.

**Why the obvious fix doesn't work:** a depth-gated "scale the far layer outward"
approach was tried and shelved. The dome has a *range* of projected depths (its
center is nearer than its edges), so depth-gating scales only its far edge and
*warps* it rather than expanding it uniformly — and pushing harder never fully
closes the corners. The clean fix is to bracket the **specific sky-draw
function** so the whole dome scales together (the same mechanism Tomba uses for
its far backdrop). That function was traced to the title's **overlay code**, so
wiring it needs the overlay-compile path too — a deliberate follow-up, not a
quick tweak. Live probes for the investigation are in the runtime
(`ws_dome` / `ws_dome_probe` / `ws_far_threshold` TCP commands).

---

## #2 — Widescreen: some objects still cull at the 4:3 edge (e.g. amusement-park ferris wheel) — OPEN

Most geometry that was clipped at the 4:3 edge now draws out to the wide edge
(the render-funnel and per-object screen culls were widened). But a few objects —
notably the **ferris-wheel cars** in the amusement-park intro — still pop in/out
at the old 4:3 boundary. They're gated by a *different* cull mechanism than the
ones already widened, which needs a live trace of that specific scene to
pinpoint and widen. Cosmetic, widescreen-only.

---

## #3 — Widescreen: distance (draw-distance) pop-in at very wide FOV — OPEN

Separate from the edge culling above: the game stops drawing objects beyond a
fixed **distance** from the camera (a classic PS1 draw-distance / far-clip cull).
A wider field of view — especially at 21:9 — lets you see farther toward that
limit, so distant objects can be seen spawning in and out by *range* rather than
at the screen edge. This is a Z/distance cull, not an aspect-ratio one, so the
edge-cull widening doesn't touch it.

**Desired:** make the draw distance configurable (push it out so the player never
sees distant objects pop in/out, in whatever aspect they're playing). That means
tracing the game's distance-cull constant(s) and widening them behind a per-game
config knob (an opt-in, since a larger draw distance costs some performance and
changes the authored look). Same investigation shape as #1/#2 — deferred with
them.

---

## #4 — Memory-card save/load does not complete — FIXED (v0.0.2)

Reaching the memory-card screen to save or load progress did not complete. The
low-level card protocol worked on both this runtime and the Beetle oracle (card
reads succeed, an empty card reports empty, the SwCARD I/O-complete flag sets and
the card fhandler runs), so this was a **higher-layer timing race**, not a dead
card path. Two nondeterministic failure modes were seen: a soft stall on
"Checking… MEMORY CARD" (the interrupt-driven async card read aborted partway),
and, less often, a hard freeze from a register smear at the card-op consumer.

**Fixed** by a framework-level change to how a cooperative in-exception
thread-switch is *deferred* across interrupt delivery. The deferred switch is now
kept pending at dirty-interpreter pump sites (where a candidate resume PC is
committed but the live CPU state may not yet be materialized) and is not deferred
at all when the interrupted PC is in low BIOS/kernel space (which would otherwise
re-enter the same VBlank handler forever and starve the target thread). The switch
is honored only at a boundary where the resumed thread's CPU state is coherent, so
the card read/write completes cleanly. Class-level fix in the framework (no
per-game poke); validated on Ape Escape and regression-checked against MegaManX6
and Tomba (1).

---

## #5 — Widescreen: HUD / UI elements are stretched — OPEN (investigated; fix attempt ABANDONED 2026-07-10)

On the projection-and-stretch widescreen path, 3D geometry is squashed at GTE
projection time so the final frame stretch restores its proportions. Ape's HUD
computes its screen coordinates CPU-side, bypassing the GTE squash, so the
stretch fattens it: HUD and UI render proportionally stretched in 16:9 / 21:9.

### What was established (evidenced — GP0 ring dumps + wtrace provenance + guest-stack walks, Fossil Field)

- **Ape's in-game HUD is mostly polygons**, not sprites: the cookie stack and
  gadget cross are gouraud-textured quads/tris (0x3C/0x34/0x3E). Only the L2
  chip and sparkle dots are SPRTs. The framework's untagged-SPRT
  `hud_sprt_squash` therefore cannot cover it, and untagged polys can't be
  blanket-squashed (the world is polys).
- **HUD packets are heap-interleaved with world packets** in one per-frame
  arena; addresses shift every frame; submission order interleaves. No
  address- or order-based classifier exists. (Exception: the pause menu uses a
  dedicated static arena around 0xCA000.)
- **The HUD drawers are invoked through function-pointer task callbacks.**
  Static `jal` call-graph analysis names a menu-time orchestrator
  (`0x8005BF70`) but NO gameplay caller — bracketing the orchestrator verified
  the mechanism (entry hook fired, stores marked) but it simply never runs
  during gameplay (2 entries total, both menu transitions).
- **World and HUD share ONE object-render funnel** (`0x80017EFC` →
  `0x800180B8`), confirmed by walking saved-`ra` slots on the guest stack as
  they were written. The funnel dispatches per object type to different vertex
  builders: `0x8001ACxx`-family (GTE-fed, world) vs `0x8001B3CC`
  (screen-space, cookie stack) vs `0x80044EF4` (2D UI sprite engine via
  `0x80043A8C`: gadget cross, pause menu, and other screen-space quads).

### What was tried and why it failed

A framework mechanism (`[widescreen] hud_bracket_funcs`) was built: the
recompiler emits an entry hook in listed functions; the runtime brackets
execution until the function returns ((sp,ra) contract checked per compiled
block), frame-stamps every RAM word stored inside the bracket, and the GPU
squashes stamped prims around a thirds-anchor pivot.

1. **Bracketing the menu orchestrator `0x8005BF70`** — mechanism worked,
   target wrong: it does not run during gameplay (see above). No effect.
2. **Bracketing the vertex builders `0x8001B3CC` + `0x80044EF4`** (plus
   `hud_sprt_squash` for the chip) — caused **garble all over the game**
   (user-observed). The builders are NOT HUD-dedicated: they emit
   screen-space packets for world-anchored content too, and everything stored
   inside the bracket window gets tagged, so world rendering corrupts.

**Conclusion: code-path discrimination cannot separate Ape's HUD from its
world.** The game funnels both through the same code; the difference lives in
the DATA (the object being drawn), not the path. The `hud_bracket_funcs`
implementation was discarded (never merged; the investigation trail lives on
the `codex/ape-widescreen-fixes` / `codex/ape-widescreen-framework` GitHub
branches).

### What a future attempt needs

Object-level or data-flow discrimination, not code-path bracketing:

- classify by the object struct handed to the render funnel (a screen-space /
  ortho flag should exist in the object header the funnel dispatches on), or
- GTE-provenance tagging: mark packet words whose values derive from GTE
  output registers (data-flow taint at the store level); squash only prims
  with NO GTE-derived vertices. Heavier, but it is the ground-truth
  definition of "bypassed the squash".

Do NOT retry: orchestrator brackets, builder brackets, address/order/arena
classifiers (all falsified above). Diagnostic tooling that survives in the
framework: the always-on GP0 ring with per-prim source addresses
(`gpu_frame_dump`), wtrace ra-slot stack walking, and the `gpu_state`
widescreen debug block.

---

## Notes

- These are **enhancement-tier** items on the experimental widescreen path.
  4:3 is the authentic default and is byte-for-byte the original presentation.
- Widescreen is offered on both the OpenGL and Software renderers; it was
  validated primarily on OpenGL (the shipping default), which also carries the
  native-wide compositor perf fix.
