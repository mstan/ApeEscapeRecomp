# Ape Escape AOT coverage

`tools/build_aot.ps1` is the reproducible play-free pipeline. It extracts every
disc producer, compiles audit-gated overlay DLLs directly into `build-aot/cache`,
builds the runtime, and refreshes the scoreboard whenever a live capture exists.
The script lowers itself and all build children to Windows BelowNormal priority.

The 2026-07-18 validation produced 47/47 clean shards: 44 HED/BNS members, two
self-describing minigame executables, and one BIOS-resident helper. A full
title-to-attract-to-title cycle exercised four overlay generations. All 72 game
PCs were covered by compiled overlay ranges; the other 35 PCs were covered by
the separately generated BIOS/kernel ranges, for 107/107 combined coverage and
zero true gaps. Live-byte guards rejected 21 stale candidates, and screenshots
verified both the rainbow/falling scene and returned title screen rendered
correctly.

Production `game.toml` writes append-only history beside the executable.
Development `game_aot.toml` additionally persists signed immutable snapshots in
the gitignored `.aot_capture_history/SCUS-94423` directory. The validation run
verified 21/21 snapshots with no invalid or duplicate references.
