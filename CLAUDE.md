# ApeEscapeRecomp Rules

Static recompilation of **Ape Escape (USA)** — serial **SCUS-94423** — to
native code, built with the shared **psxrecomp v4** framework. The end goal is
a binary that plays without an emulator behind it, exactly like TombaRecomp.

## Inheritance

This project inherits, in order:

1. `F:/Projects/recomp-template/PRINCIPLES.md` — system-agnostic recomp/debug
   discipline (ground truth = original EXE + emulator oracle; generated C is
   evidence, not authority; first-divergence; no guessing).
2. The framework constitution at `psxrecomp-v4/CLAUDE.md` — a junction →
   `F:/Projects/psxrecomp/psxrecomp`. Read it first: no MIPS interpreter, no
   HLE BIOS shims, no stubs, recompiled-BIOS-first, fix the
   framework/runtime/config and **regenerate** — never hand-edit `generated/`.

## Project rules

- Game binaries (disc image, extracted boot EXE, the headerless Ghidra dump),
  Ghidra databases, memory cards, and build outputs are **local only** and must
  not be committed. See `.gitignore`.
- Tracked: `game.toml`, `seeds/`, `annotations/`, `ghidra/instructions.txt`,
  `ghidra/scripts/`, `ghidra/annotations/`, `CMakeLists.txt`, `tools/`, docs.
- Codegen/runtime fixes belong in the framework (`psxrecomp-v4/`) or in
  per-game `game.toml` config — never in `generated/*.c`. A fix that only this
  game needs is a smell; prefer a class fix that the next title inherits.
- After every run, resolve all dispatch misses before any other debugging.
- The framework version this project builds against is pinned in
  `psxrecomp-v4.pin`.
