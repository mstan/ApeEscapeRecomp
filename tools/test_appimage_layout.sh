#!/usr/bin/env bash
# test_appimage_layout.sh — verify a built recomp AppImage seeds the
# writable data directory correctly, without launching the game.
#
# Runs the AppImage with <ENV_PREFIX>_SEED_ONLY=1, which makes AppRun perform
# its seeding and print the data directory instead of exec'ing the runtime.
# Then asserts the layout the release promises.
#
# Usage: bash tools/test_appimage_layout.sh path/to/<Title>-<ver>-linux-x86_64.AppImage
set -euo pipefail

appimage=${1:-}
[ -n "$appimage" ] || { echo "usage: $0 <AppImage>" >&2; exit 2; }
[ -x "$appimage" ] || { echo "not executable: $appimage" >&2; exit 1; }

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
expected_version=$(tr -d ' \t\r\n' < "$root/packaging/release/VERSION")
# Per-game identity: ENV_PREFIX names the AppImage's env overrides.
# shellcheck source=/dev/null
. "$root/packaging/release/app.conf"

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

data_dir=$(env "${ENV_PREFIX}_DATA_DIR=$work/data" "${ENV_PREFIX}_SEED_ONLY=1" \
    "$appimage" --appimage-extract-and-run)
[ -n "$data_dir" ] || { echo "AppRun printed no data dir" >&2; exit 1; }

fail=0
check_file() { [ -f "$data_dir/$1" ] || { echo "MISSING file: $1" >&2; fail=1; }; }
check_dir()  { [ -d "$data_dir/$1" ] || { echo "MISSING dir:  $1" >&2; fail=1; }; }

for d in saves cache mods mods/bundled assets bios; do check_dir "$d"; done
for f in game.toml input.ini START_HERE.txt LICENSE README.md \
         bios/openbios.bin bios/OpenBIOS.LICENSE .appimage-layout-version; do
    check_file "$f"
done

got_version=$(tr -d ' \t\r\n' < "$data_dir/.appimage-layout-version")
if [ "$got_version" != "$expected_version" ]; then
    echo "version marker mismatch: AppImage says '$got_version', VERSION says '$expected_version'" >&2
    fail=1
fi

legacy_packages=$(find "$data_dir/mods" -path "$data_dir/mods/packages" -type d | wc -l)
if [ "$legacy_packages" -ne 0 ]; then
    echo "seeded legacy mods/packages catalog" >&2
    fail=1
fi
package_dirs=$(find "$data_dir/mods/bundled" -mindepth 1 -maxdepth 1 -type d | wc -l)
manifests=$(find "$data_dir/mods/bundled" -mindepth 2 -maxdepth 2 -name manifest.toml | wc -l)
if [ "$package_dirs" -eq 0 ] || [ "$manifests" -ne "$package_dirs" ]; then
    echo "seeded mod catalog has $package_dirs package dir(s) and $manifests manifest(s)" >&2
    fail=1
fi

# The bundled overlay cache must arrive as Linux .so shards under
# gcc/linux-x64; a .dll here would mean the Windows cache was staged and the
# loader (which dlopen()s OVERLAY_SHARED_EXT) would ignore every one of them.
seeded_so=$(find "$data_dir/cache" -name '*.so' 2>/dev/null | wc -l)
if [ "$seeded_so" -eq 0 ] && [ "${ALLOW_NO_CACHE:-0}" != "1" ]; then
    echo "seeded cache holds no .so shards" >&2
    fail=1
fi
# Count rather than pipe into `grep -q`: grep exits on its first match and
# SIGPIPEs find, and under `set -o pipefail` that makes the whole pipeline
# report failure -- so a NEGATED grep -q test fires precisely when the thing
# it is looking for IS present. That false alarm cost a debug cycle here.
stray_dll=$(find "$data_dir/cache" -name '*.dll' | wc -l)
if [ "$stray_dll" -ne 0 ]; then
    echo "seeded cache contains $stray_dll Windows .dll shards; the Linux loader cannot use them" >&2
    fail=1
fi
arch_so=$(find "$data_dir/cache" -path '*/linux-x64/*' -name '*.so' | wc -l)
if [ "$seeded_so" -gt 0 ] && [ "$arch_so" -eq 0 ]; then
    echo "cache .so shards are not under a linux-x64 arch-abi directory" >&2
    fail=1
fi

# A retail BIOS or disc image must never be inside the payload.
stray=$(find "$data_dir" \( -iname 'SCPH*.BIN' -o -iname '*.cue' -o -iname '*.iso' \
        -o -iname '*.mcd' \) -print 2>/dev/null || true)
if [ -n "$stray" ]; then
    echo "payload contains files that must never ship:" >&2
    printf '  %s\n' $stray >&2
    fail=1
fi

# Seeding must be idempotent: a second run must not fail or duplicate.
env "${ENV_PREFIX}_DATA_DIR=$work/data" "${ENV_PREFIX}_SEED_ONLY=1" \
    "$appimage" --appimage-extract-and-run >/dev/null

# A shard the player's own session built must survive a reseed: AppRun seeds
# the cache with cp -n, so release shards fill gaps without overwriting.
user_shard=$data_dir/cache/.user-shard-probe
printf 'player-built\n' > "$user_shard"
env "${ENV_PREFIX}_DATA_DIR=$work/data" "${ENV_PREFIX}_SEED_ONLY=1" \
    "$appimage" --appimage-extract-and-run >/dev/null
if [ ! -f "$user_shard" ] || [ "$(cat "$user_shard")" != "player-built" ]; then
    echo "reseed destroyed a player-built cache entry" >&2
    fail=1
fi

# User-owned files must survive a reseed.
echo "; user edit" >> "$data_dir/input.ini"
before=$(sha256sum "$data_dir/input.ini" | awk '{print $1}')
env "${ENV_PREFIX}_DATA_DIR=$work/data" "${ENV_PREFIX}_SEED_ONLY=1" \
    "$appimage" --appimage-extract-and-run >/dev/null
after=$(sha256sum "$data_dir/input.ini" | awk '{print $1}')
if [ "$before" != "$after" ]; then
    echo "reseed clobbered user-owned input.ini" >&2
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "AppImage layout test FAILED" >&2
    exit 1
fi
echo "AppImage layout test passed ($expected_version)"
