# Disc identity — Ape Escape (USA)

Redump-verified clean dump. Format: **bin/cue, single track, MODE2/2352, NTSC-U**.
Do **not** convert to ISO — a 2048-byte "cooked" ISO discards the Mode-2 Form-2
XA sectors PSX uses for streaming FMV/audio.

| Field | Value |
|-------|-------|
| Title | Ape Escape (USA) |
| Serial | SCUS-94423 |
| Track | 01, MODE2/2352, data |
| Size (.bin) | 432,238,800 bytes |
| CRC32 | `C6F455BC` (per Redump) |
| MD5 | `DE4E7AB78C08BD03712E83C14D4CF642` |
| SHA-1 | `466CCE4BCD6992F57227ABD270323BCDAD2FB7FC` |

Verified 2026-05-28: locally computed SHA-1/MD5 match the Redump database entry
for Ape Escape (USA). This is the only USA release — no revisions.

Boot EXE: `SCUS_944.23` — load `0x80010000`, entry `0x800A3660`, text `0xA5000`.
Disc image and extracted EXE are local-only (gitignored); recreate from the
source dump if missing.
