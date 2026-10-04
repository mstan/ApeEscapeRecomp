# Optional mouse gadget controls

This feature is off by default. The LEFT-held revision passes six focused tests,
six native SIO checks and three host binding tests on each SDL backend, plus
five registered game tests after a full incremental SDL3 Release link. Its
changed SDL adapters compile with SDL2 and SDL3; the unchanged main/plugin
path retains the preceding paired-source compilation. Static import/profile
checks pass for the new private candidate. The preceding candidate passed a
visible-window boot smoke. The owner
reported that its mouse gestures worked pretty well. Physical acceptance of
this revised activation behavior and the complete interruption matrix remain
pending. It requires the paired psxrecomp
local-mouse policy API; the release framework pin alone does not contain it.
The current LEFT-held revision has not been launched for physical validation.
An ordinary mod archive cannot add the missing native implementation to stock
v0.5.0; a release containing the paired runtime hooks and game plugin is required.

## Use and tuning

In the launcher's Mods page, enable **Ape Escape Mouse Gadget Controls**.
Start local gameplay with P1 in analog mode, focus the game window, then
**hold the left mouse button**. Mouse gadget control is active only while held;
release immediately neutralizes its contribution and returns to ordinary input.
The owned left press and release are consumed before binding folding. Neither
edge displays a routine popup. Press alone is neutral; fresh motion is required.
Move while held: a directional stroke deflects the right stick, and
continued clockwise/counterclockwise circles turn the vector in that sense.
The left stick and ordinary PSX button bindings remain available.

Escape releases capture and is consumed through its release. A native right
stick takes over as a whole vector and releases capture. Focus loss,
hide/minimize, menus, state restore/rewind, reset, routing changes and a host
stall also release it. Holding left through an interruption cannot silently
recapture: release, press again and provide fresh motion after returning.
Disable the mod to use the normal native input path throughout.

Sensitivity defaults to 100% (48 relative units for full radial displacement),
with a 25–400% option. SDL relative units depend on the mouse, DPI, platform
and backend; they are not physical distances. Axis inversion is optional;
positive X means right and positive Y means down by default.

The hold option is **Mouse3** (right mouse), **LeftAlt**, or **None**. After
capture, press the hold control and then move. Press alone is neutral. Motion
updates the held direction; releasing immediately neutralizes it and requires
fresh motion. A control already held while capture is acquired must first be
released and pressed again. A conflicting primary or alternate P1 PSX binding
disables hold interpretation and shows a notice. The binding is preserved.
An ignored hold-control release does not cancel an ordinary mouse flick.
This direction hold is separate from activation: it can never retain capture
or gadget input after the left button is released. Capture failure and a
conflicting direction-hold binding retain their actionable, bounded diagnostics.

The candidate filter uses a 2-unit accumulated noise gate, 100 ms event-time
decay, an 80 ms flick pulse and an 80 ms return. Direction receives a
first-order correction for half the preceding packet interval, capped at
4 ms, only when real motion arrives. Equal-time splits share the same prior
event; the displacement gate and amplitude are unchanged. Correction is
disabled after the 160 ms pulse/return history has gone quiet. Its vector is
limited to half the current displacement magnitude, so corrected direction
cannot reverse that displacement or collapse near zero before normalization.
The corrected angle stays within 30 degrees of the displacement direction.
Quiet samples keep the direction fixed and cannot predict further rotation.

Smooth 80-unit-radius synthetic circles at 0.5/1/2 Hz passed the unchanged
<=5% gain and <=5-degree phase assertions across 125-8000 Hz polling traces.
Measured maximum gain differences were 4.351%, 4.185% and 0.000%; maximum
phase differences were 0.699, 1.397 and 2.782 degrees. These are comparisons
between available sampled traces, not physical-device results. Real packet
jitter, coalescing, DPI, backend behavior and gadget feel remain unmeasured.
Decay can produce reversal lag; the constants are not claimed optimal.

This controls right-stick gadgets while retaining the rest of the pad. It does
not replace every control in vehicles or dual-stick minigames. It is inert
for netplay, replay/resimulation, headless/debug injection and host menus.
Unknown guest contexts fail closed. Supported gadget/minigame coverage must
be confirmed with the actual game before public-readiness acceptance.

## Reducer and input contract

The pure reducer receives timestamped fractional displacement. It decays its
accumulated displacement only at real motion events, adds the displacement,
and recomputes direction and magnitude on every real update. Below-gate
cancellation clears the candidate. Zero motion cannot renew the pulse.
Const samples do not consume motion, change anchors or renew deadlines.
There are no synthetic extra turns. Without hold, quiet output reaches exact
128/128 after 160 ms. The existing radial stick-to-byte mapper is reused
with no second physical-controller deadzone.

The runtime delivers motion/control events synchronously on SDL's owner
thread. It owns relative capture, lifecycle resets, timestamp checks and
source suppression. Only P1 RX/RY may be overridden, after native folding
and controller presentation and before normal pad/SIO delivery. Native type,
buttons, left axes and other seats are retained. Acquisition and captured
Escape are suppressed as host sources before binding folding; a held gamepad
button is never removed from the merged report.

## Source-derived guest eligibility

These observations are from the USA SCUS-94423 boot executable, read from an
existing owned disc without modifying it or saving its executable bytes.
They are persistent state reads, not a Start-toggle approximation.

| State/guard | Read | Source evidence | Current decision |
|---|---|---|---|
| Top-level dispatcher | halfword 800F4470 | 8005BDC0 establishes state base 800F4350; 8005BE04 indexes table 800AFA30 in 8-byte entries; 8005BE98–8005BEB4 dispatch through its function pointer | Allow only table entries 0B, 0D, 11, 13, 18, 22, 24, all pointing to common interactive loop 8005BF70 |
| Interactive phase | byte 800F447C | Shared interactive predicate 8005DC78 uses LBU at 8005DC88 | Require 3–5; alone this is insufficient |
| Persistent menu state | byte 800BEF74 | Entry 8007F56C stores 1 at 8007F5C4; 8007F5D4 returns it; 800821D4 branches on it; 8007FCF4 dispatches menu states; 80080254/800810DC clear it | Any nonzero value releases capture |
| Alternate scene path | word 800F4450 | 8005C108–8005C138 select an alternate path on bit 0 | Fail closed while set; full semantic classification pending |
| Actor state | halfword 800EC23E | 8005DC78–8005DCE0 exclude 23, 4A, 2D | Apply the game's exclusions; exact animation/cutscene meanings pending |
| Actor flags | word 800EC250 | Same interactive predicate rejects low-nibble group E0 under mask F0 | Require (flags & F0) != E0 |
| Pad mode | byte 800B87A0 | Existing game controller/Quick Gadget Select source evidence | Require analog pad ID 73 |
| Gadget/unlock | byte 800EC2D2 / byte 800F51C4 | Existing gadget source evidence | Require gadget <8 with its unlock bit |
| Menu-entry buttons | Current native active-low pad word | Interactive loop 8005C300–8005C35C calls 8007F56C with menu selector 1 or 0A | Suspend for Start/Select before that report can enter a menu; processed internal masks require gameplay cross-check |
| Card/overlay/unknown dispatchers | Same table | Card-menu entry 0E points to overlay 80136AB8, unlike the common loop | Reject |
| Restore/rewind | Runtime lifecycle plus restored RAM | Runtime callbacks reset capture; predicate rereads current guest state | Never carry a prior capture across restoration |

This proves the storage and dispatch relationships used by the guard. It
does not yet establish an observed truth table covering every named level,
transition, cutscene, gadget menu or minigame. Verification must demonstrate
capture in real gameplay and refusal/release in each interruption context.
Quick Gadget Select's code and guest pad shadow RAM are untouched.

## Verification

The focused CMake project avoids generated game code, BIOS, SDL and rendering:

```sh
cmake -S tests/mouse_gadgets -B build/mouse-focused -DPSXRECOMP_ROOT=/path/to/reviewed/psxrecomp
cmake --build build/mouse-focused --parallel 2
ctest --test-dir build/mouse-focused --output-on-failure
```

It registers gesture, context, generic policy, existing stick-response and
catalog tests. Gesture sources cover eight strokes, off-axis/reversal,
cancellation, fractional/equal-time splits, pulse/quiet return, hold/release,
circle amplitude/winding and a figure-eight. Exact timestamped-history checks
use common samples with 30/60/120/144/240 Hz query schedules, batching and
repeated early/late queries. Separate 125–8000 Hz circle approximations print
gain and phase; they are not interchangeable with exact-history invariants.

The runtime's SDL2/SDL3 CTest configuration also registers actual source-fold
mouse suppression and keyboard chord regressions. These use controlled host
state without creating a window. Source suppression through release, ordinary
bindings, alternate bindings, P2 and simultaneous controller actions are checked
before folding. The private compile harness also builds the real adapter,
plugin/reducer and main translation unit without linking or running the game.

The five focused targets passed with MSVC 19.50, CMake 4.1.2 and Ninja 1.12.1
on the paired release-base copies. This includes existing stick-response and
catalog checks, plus reducer timing/geometry, source context and generic
capture policy regressions. Sixty direction-correction cases cover quiet-gap
fresh strokes, near-zero extrapolation and direction-inconsistent correction,
including both axes, mirrored signs and equal-time packet splits. All prior
gain, phase, timing and geometry assertions remain in place and pass.
The activation integration test couples the real reducer to the generic runtime
policy, checking neutral presses, held motion, both circle senses and useful
amplitude, release/rapid repress, right hold at left release and interruption
gates. Generic policy and source-fold tests cover silent edges and primary/alternate
left bindings without deleting simultaneous controller or P2 actions.
Six native SIO checks also pass, including a 24-assertion regression for P1
wire byte order, P2 preservation and pad-type changes at the idle bus boundary.
The memory-card fixture now creates its scratch directory without a shell on
Windows and POSIX. These checks use the actual SIO implementation; they do not
execute the main loop's host-to-SIO delivery.

With official SDL3 3.4.10 and SDL2 2.32.10 VC x64 development packages, all three
host tests pass separately on both backends: host keymap, keyboard pad chord
(including captured Escape/Turbo) and mouse binding suppression. Conflicting
binding fixtures load through the actual INI path, which permits existing
duplicate bindings; the interactive rebinding setter moves a key instead.
All original assertions remain. Both backend paths compile local_mouse_sdl.cpp,
the game plugin/reducer and actual main.cpp in a no-launcher/no-netplay/no-Vulkan
configuration. The private MSVC compile harness selects /Zc:lambda for the
existing constexpr presentation guard; that guard is retained. This does not
establish a full runtime/game configuration or link, nor execute actual capture
or the main loop. The first fixture failures and initial main compile error are
preserved in the private verification record.
Actual net/club activation, rotations, sustained hold/slingshot use,
Quick Gadget Select coexistence, physical feel and interruptions are not run.
Synthetic circles cannot establish gadget usefulness or device feel.

## Bases and paired delivery

Game base: v0.5.0, a8e219ab48e140a84d588374bcd2ca43dda70d70.
Runtime base: 065888f50f9131839bcbbc8814debf58b4624b16, the release's
actual gitlink and packaging record. `framework_pins.txt` is reconciled to
that base; f7f0ad1097178d5df01a16eafe030f435565ed64 was stale.
Their only runtime difference is freeze diagnostics, with the relevant input
and plugin interfaces unchanged.

The final feature gitlink is intentionally pending the paired runtime PR.
Before updating it, establish reachability through the configured framework
upstream, update all actual pin records together, and repeat checkout/build/CI
on those final bytes. A feature commit reachable only from a contributor fork
does not satisfy that upstream gate. Current framework master also differs
from the release's existing mod APIs; any rebase must be reviewed and retested.
