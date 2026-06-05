# ApeEscapeRecomp

Static recompilation of **Ape Escape (USA)** (SCUS-94423) to native code,
built on the shared **psxrecomp v4** framework — the same pipeline behind
TombaRecomp. The goal is a native binary that runs the game with no emulator
behind it.

## Status

**Scaffolded.** Project layout, config, the disc image, the extracted boot EXE,
and the Ghidra dump are all in place. Not yet recompiled or booting — that is
the next phase.

## Required user-owned assets (not included in the repo)

- PlayStation BIOS `SCPH1001.BIN` — provided by the framework at
  `psxrecomp-v4/bios/SCPH1001.BIN`.
- The Ape Escape (USA) disc image (`apeescape/Ape Escape (USA).cue` + `.bin`)
  and the extracted boot EXE `apeescape/SCUS_944.23`. These are local-only and
  gitignored.

## Layout

| Path | Purpose |
|------|---------|
| `game.toml` | Game identity + recompiler/runtime config (entry point, load address, text size, disc path). |
| `apeescape/` | Disc image + extracted boot EXE `SCUS_944.23` + `SYSTEM.CNF` (local). |
| `seeds/` | Function-start seeds fed to the recompiler. |
| `annotations/` | CSV of human notes emitted as comments in the generated C. |
| `ghidra/` | Headerless dump + `instructions.txt` for reverse engineering. |
| `generated/` | Recompiled C (local; produced by `tools/regen.ps1`). |
| `psxrecomp-v4` | Default junction to the shared framework. Override with `PSXRECOMP_ROOT` or CMake `-DPSXRECOMP_ROOT=...` for isolated worktrees. |

## Build (from source)

```
pwsh tools/regen.ps1 -PSXRECOMP_ROOT F:/Projects/psxrecomp/_wt-ape-fw
cmake -S . -B build -G "Unix Makefiles" -DPSXRECOMP_ROOT=F:/Projects/psxrecomp/_wt-ape-fw
cmake --build build -j16
./build/psx-runtime.exe --game game.toml
```

(The framework recompiler `psxrecomp-v4/recompiler/build/psxrecomp-game.exe`
must be built first; see the framework's own README.)

## Disc identity

`MODE2/2352` bin+cue, single data track, NTSC-U. Hashes recorded in
`ghidra/instructions.txt` siblings / project notes; verify a fresh dump matches
before blaming a regression.

## Rules

See `CLAUDE.md`. In short: fixes go in the framework or `game.toml`, never in
`generated/`; no stubs; binaries stay local.
