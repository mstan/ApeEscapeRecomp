#!/usr/bin/env bash
# package_appimage.sh -- build the Linux x86_64 AppImage release for a recomp title.
#
# Counterpart to tools/package_release.ps1 (Windows). Both packagers read the
# SAME sources so the two platforms cannot drift:
#
#   packaging/release/VERSION      the version string (single source of truth)
#   packaging/release/game.toml    the player-facing config
#   packaging/release/input.ini    the default controller mapping
#   packaging/release/START_HERE.txt
#
# Overlay cache releases are also expected to share capture evidence:
# Windows and Linux shards must be rebuilt for their own platform, but both
# should be compiled from the same validated overlay_captures.json manifest.
# Do not copy Windows .dll shards into an AppImage; rebuild .so shards from
# that manifest with compile_overlays.py.
#
# Reproducibility:
#   * the version is never hardcoded here or in AppRun (AppRun's marker is
#     stamped from VERSION at package time),
#   * linuxdeploy/appimagetool are pinned by URL + SHA256 and verified,
#   * SOURCE_DATE_EPOCH is derived from the git commit (override to pin it),
#     and every staged file's mtime is normalised to it before squashing,
#   * the run prints SHA256 for the artifact.
#
# WSL: building an AppDir directly on a /mnt/<drive> DrvFs mount fails or
# silently degrades; symlinks need metadata mount options and the exec bit is
# not preserved. When the repo lives on /mnt we therefore stage the AppDir on
# the native Linux filesystem and copy only the finished .AppImage back to the
# Windows-visible output path. Pass --out to place it elsewhere; a Windows-style
# path (F:\... or F:/...) is translated with wslpath.
#
# Usage:
#   bash tools/package_appimage.sh                     # version from VERSION
#   bash tools/package_appimage.sh --version v0.0.7
#   bash tools/package_appimage.sh --out /mnt/f/drop   # or --out 'F:\drop'
#   bash tools/package_appimage.sh --skip-build        # reuse existing build dir
#
# Prereqs: cmake, ninja or make, a C/C++ toolchain, libsdl2-dev,
# libgl1-mesa-dev, curl, ImageMagick (for the icon), and a generated/ tree
# (the recompiler runs on Windows; generated C is committed).
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# Keep the original argv: the parse loop below shifts it away, and the
# low-priority re-exec has to pass the caller's arguments through intact.
orig_args=("$@")

version=""
out_dir=""
skip_build=0
build_dir=${BUILD_DIR:-"$root/build-appimage"}
# Leave two cores for the rest of the machine; packaging must not make the box
# unusable. Override with --jobs / BUILD_JOBS.
_cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
jobs=${BUILD_JOBS:-$(( _cores > 4 ? _cores - 2 : 2 ))}

while [ $# -gt 0 ]; do
    case "$1" in
        --version) version=$2; shift 2;;
        --out)     out_dir=$2; shift 2;;
        --build-dir) build_dir=$2; shift 2;;
        --jobs)    jobs=$2; shift 2;;
        --skip-build) skip_build=1; shift;;
        --nice) nice_level=$2; shift 2;;
        -h|--help) sed -n '2,36p' "$0"; exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

# Packaging a release should not make the machine unusable. Re-exec the whole
# script under `nice` once (children inherit it) unless already niced or told
# otherwise with --nice 0.
nice_level=${nice_level:-10}
if [ "$nice_level" -gt 0 ] && [ "${RECOMP_APPIMAGE_RENICED:-0}" != "1" ] \
   && command -v nice >/dev/null 2>&1; then
    export RECOMP_APPIMAGE_RENICED=1
    exec nice -n "$nice_level" "$0" ${orig_args[@]+"${orig_args[@]}"}
fi

[ -n "$version" ] || version=$(tr -d ' \t\r\n' < "$root/packaging/release/VERSION")
[ -n "$version" ] || { echo "empty version" >&2; exit 1; }

# Per-game identity (see packaging/release/app.conf). Everything below this
# point is title-neutral, so porting this script to another title is a copy
# plus that one file.
app_conf=$root/packaging/release/app.conf
[ -f "$app_conf" ] || { echo "missing $app_conf" >&2; exit 1; }
# shellcheck source=/dev/null
. "$app_conf"
for v in APP_NAME EXE_NAME PAYLOAD_DIR DESKTOP_ID ENV_PREFIX ICON_SOURCE FRAMEWORK_DIR; do
    eval "val=\${$v:-}"
    [ -n "$val" ] || { echo "$app_conf does not set $v" >&2; exit 1; }
done

# --- path handling ---------------------------------------------------------
# Accept Windows-style output paths so the same command works from a WSL shell
# driven by a Windows tool.
to_unix_path() {
    case "$1" in
        [A-Za-z]:[/\\]*)
            if command -v wslpath >/dev/null 2>&1; then wslpath -u "$1"; else
                echo "cannot translate Windows path '$1' (wslpath missing)" >&2; exit 2
            fi;;
        *) printf '%s\n' "$1";;
    esac
}
[ -n "$out_dir" ] && out_dir=$(to_unix_path "$out_dir")
out_dir=${out_dir:-"$root/release-linux"}
mkdir -p -- "$out_dir"
out_dir=$(CDPATH= cd -- "$out_dir" && pwd)

is_wsl=0
if [ -r /proc/version ] && grep -qiE 'microsoft|wsl' /proc/version; then is_wsl=1; fi

# DrvFs (/mnt/<drive>) cannot host the AppDir: linuxdeploy needs real symlinks
# and preserved exec bits. Stage on the native filesystem and copy the finished
# artifact back.
stage_base=$build_dir
case "$root" in
    /mnt/*)
        if [ "$is_wsl" = "1" ]; then
            stage_base=${TMPDIR:-/tmp}/$PAYLOAD_DIR-appimage.$$
            echo "WSL: repo is on a DrvFs mount; staging AppDir at $stage_base"
        fi;;
esac
appdir=$stage_base/AppDir
tools_dir=${RECOMP_APPIMAGE_TOOLS:-"${XDG_CACHE_HOME:-$HOME/.cache}/recomp-appimage-tools"}
output=$out_dir/$EXE_NAME-$version-linux-x86_64.AppImage

cleanup() {
    [ -n "${derived_toml:-}" ] && rm -f -- "$derived_toml"
    case "$stage_base" in
        /tmp/"$PAYLOAD_DIR"-appimage.*|"${TMPDIR:-/tmp}"/"$PAYLOAD_DIR"-appimage.*)
            rm -rf -- "$stage_base";;
    esac
}
trap cleanup EXIT

if [ -z "$(ls "$root"/generated/*_dispatch.c 2>/dev/null)" ]; then
    echo "Missing generated game sources (generated/*_dispatch.c)." >&2
    echo "Run the recompiler first: $FRAMEWORK_DIR/recompiler/build*/psxrecomp-game --config game.toml" >&2
    exit 1
fi

# --- reproducibility anchor ------------------------------------------------
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    # A git worktree checked out by Windows stores an absolute Windows gitdir
    # path in .git, which WSL's git cannot follow ("not a git repository:
    # /mnt/f/...F:/..."), so this legitimately fails here. Fall back to the
    # VERSION file's mtime and say so, rather than silently stamping epoch 0.
    SOURCE_DATE_EPOCH=$(git -C "$root" log -1 --format=%ct 2>/dev/null || true)
    if [ -z "$SOURCE_DATE_EPOCH" ]; then
        SOURCE_DATE_EPOCH=$(stat -c %Y "$root/packaging/release/VERSION" 2>/dev/null || echo 0)
        echo "note: git date unavailable here; SOURCE_DATE_EPOCH from packaging/release/VERSION mtime." >&2
        echo "      Pass SOURCE_DATE_EPOCH explicitly to pin it across machines." >&2
    fi
fi
export SOURCE_DATE_EPOCH
echo "version=$version  SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"

# --- BIOS backends ---------------------------------------------------------
# The runtime will not configure without at least one recompiled BIOS under
# psxrecomp-v4/generated. A clean checkout has none, so generate the bundled
# OpenBIOS (MIT, no dump required) and SCPH1001 when a dump is present, rather
# than failing at cmake with a message about a script we could have run.
fw=$root/$FRAMEWORK_DIR
# Shared release staging surface: tag derivation, cache selection, toolchain
# staging, and mod catalog checks live in psxrecomp, not this title packager.
# shellcheck source=/dev/null
. "$fw/tools/release_overlay_stage.sh"
psx_release_stage_init "$fw"
# A CMake build directory records absolute paths and its generator's compiler,
# so a tree configured by Windows cmake (F:/..., ninja.exe) cannot be reused
# from WSL. Keep a Linux-only directory; never share build-t2 with Windows.
bios_build=${PSXRECOMP_BIOS_BUILD:-recompiler/build-linux}

# Generate a backend only when its ROM is actually present. Gating on the
# profile .toml instead would try to recompile SCPH1001 on any checkout, since
# the profile ships even when the dump does not.
bios_rom_for() {
    case "$1" in
        OpenBIOS) printf '%s\n' "$fw/bios/openbios.bin";;
        *)        printf '%s\n' "$fw/bios/$1.BIN";;
    esac
}
needed_stems=""
for stem in OpenBIOS SCPH1001; do
    [ -f "$fw/bios/$stem.toml" ] || continue
    [ -f "$(bios_rom_for "$stem")" ] || continue
    [ -f "$fw/generated/${stem}_dispatch.c" ] && continue
    needed_stems="$needed_stems $stem"
done

if [ -n "$needed_stems" ]; then
    if [ ! -x "$fw/$bios_build/psxrecomp-bios" ]; then
        gen=Ninja
        command -v ninja >/dev/null 2>&1 || gen="Unix Makefiles"
        cmake -S "$fw/recompiler" -B "$fw/$bios_build" -G "$gen" -DCMAKE_BUILD_TYPE=Release
        cmake --build "$fw/$bios_build" --target psxrecomp-bios -j "$jobs"
    fi
    for stem in $needed_stems; do
        echo "Generating recompiled BIOS backend: $stem"
        ( cd "$fw" && PSXRECOMP_BIOS_BUILD="$bios_build" \
            tools/regen_bios.sh --config "bios/$stem.toml" )
    done
fi

# --- build -----------------------------------------------------------------
if [ "$skip_build" = "0" ]; then
    generator=Ninja
    command -v ninja >/dev/null 2>&1 || generator="Unix Makefiles"
    # --build-id=none keeps the ELF a function of its sources: the default
    # build-id is a hash that also folds in link-time inputs and makes two
    # otherwise identical builds differ.
    cmake -S "$root" -B "$build_dir" -G "$generator" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPSX_SDL_BACKEND=SDL2 \
        -DPSX_DEBUG_TOOLS=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-Wl,--build-id=none"
    cmake --build "$build_dir" --target psx-runtime -j "$jobs"
fi

elf=$build_dir/$EXE_NAME
[ -f "$elf" ] || elf=$build_dir/psx-runtime
[ -f "$elf" ] || { echo "no runtime ELF under $build_dir" >&2; exit 1; }
file -b "$elf" | grep -q ELF || { echo "$elf is not an ELF binary" >&2; exit 1; }

# --- player game.toml ------------------------------------------------------
# Two conventions exist across the titles and both are honoured:
#   packaging/release/game.toml present -> ship it verbatim (Tomba 2: a
#       curated player config, deliberately smaller than the dev one)
#   absent -> derive from the repo's real game.toml by cutting the dev-only
#       audit block (Ape Escape: one source of truth, so the shipped config
#       cannot drift from the config that was validated)
# The codegen tag is computed from whichever file results, so the bundled
# cache always matches what the player actually runs.
player_toml=$root/packaging/release/game.toml
derived_toml=""
if [ ! -f "$player_toml" ]; then
    [ -f "$root/game.toml" ] || { echo "no packaging/release/game.toml and no game.toml" >&2; exit 1; }
    derived_toml=${TMPDIR:-/tmp}/player-game.$$.toml
    awk '
        /Audit-specific/ { exit }
        /^\[audit\]/     { exit }
        { print }
    ' "$root/game.toml" > "$derived_toml"
    # Drop a trailing run of blank/comment lines left by the cut.
    sed -i -e :a -e '/^[[:space:]]*$/{$d;N;ba' -e '}' "$derived_toml"
    player_toml=$derived_toml
    echo "player game.toml derived from game.toml (audit block stripped)"
fi

# --- release identity: game id + codegen tag -------------------------------
# The cache namespace the loader will read is
# <game_id>/gcc/<arch-abi>/<cg_tag>_f<overlay-flavor>.
# Compute the tag exactly the way compile_overlays.py and the Windows packager
# do -- from the runtime includes plus the PACKAGED game.toml, so a cache built
# against the dev config (a different tag) is never mistaken for a usable one.
game_id=$(sed -n 's/^[[:space:]]*id[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$player_toml" | head -1)
[ -n "$game_id" ] || { echo "could not read [game] id from $player_toml" >&2; exit 1; }

recompiler_bin=$fw/$bios_build/psxrecomp-game
[ -x "$recompiler_bin" ] || recompiler_bin=$fw/recompiler/build-linux/psxrecomp-game
if [ ! -x "$recompiler_bin" ]; then
    recompiler_build=$(dirname -- "$recompiler_bin")
    gen=Ninja
    command -v ninja >/dev/null 2>&1 || gen="Unix Makefiles"
    cmake -S "$fw/recompiler" -B "$recompiler_build" -G "$gen" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$recompiler_build" --target psxrecomp-game -j "$jobs"
fi
cg_tag=$(psx_overlay_cg_tag \
    --runtime-include "$fw/runtime/include" \
    --recompiler "$recompiler_bin" \
    --game-toml "$player_toml" \
    --flavor-from-build "$build_dir" \
    --runtime-target psx-runtime)
[ -n "$cg_tag" ] || { echo "could not compute codegen tag" >&2; exit 1; }
echo "game=$game_id  codegen tag=$cg_tag"
# --- stage AppDir ----------------------------------------------------------
rm -rf -- "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/$PAYLOAD_DIR"
payload=$appdir/usr/share/$PAYLOAD_DIR

install -m 0755 "$elf" "$appdir/usr/bin/$EXE_NAME"

# AppRun carries the version marker; stamp it rather than hardcoding.
sed -e "s|@VERSION@|$version|g" -e "s|@APP_NAME@|$APP_NAME|g" \
    -e "s|@EXE_NAME@|$EXE_NAME|g" -e "s|@PAYLOAD_DIR@|$PAYLOAD_DIR|g" \
    -e "s|@ENV_PREFIX@|$ENV_PREFIX|g" \
    "$root/packaging/linux/AppRun" > "$appdir/AppRun"
chmod 0755 "$appdir/AppRun"
install -m 0644 "$root/packaging/linux/$DESKTOP_ID.desktop" \
    "$appdir/$DESKTOP_ID.desktop"

for tree in assets bios; do
    [ -d "$build_dir/$tree" ] || { echo "build did not stage $tree/" >&2; exit 1; }
    cp -a "$build_dir/$tree" "$payload/$tree"
done

psx_add_mod_catalog --build-path "$build_dir" --stage "$payload" \
                    --runtime-target psx-runtime
# OpenBIOS must ride along with its notice; a retail BIOS must not.
[ -f "$payload/bios/openbios.bin" ] || { echo "missing bundled OpenBIOS" >&2; exit 1; }
[ -f "$payload/bios/OpenBIOS.LICENSE" ] || { echo "missing OpenBIOS notice" >&2; exit 1; }

mkdir -p "$payload/licenses"
if [ -f "$fw/runtime/licenses/libchdr-NOTICES.txt" ]; then
    cp "$fw/runtime/licenses/libchdr-NOTICES.txt" "$payload/licenses/"
fi

# --- prebuilt overlay cache + overlay toolchain ---------------------------
# The cache namespace and toolchain layout are framework-owned. The cache source
# root is the parent of the per-game directory, matching compile_overlays.py
# --out-dir and the Windows packager.
cache_src_root=${OVERLAY_CACHE_DIR:-"$root/build-linux-cache/cache"}
case "$cache_src_root" in
    *QUARANTINE*) echo "refusing quarantined overlay cache source: $cache_src_root" >&2; exit 1 ;;
esac
psx_add_overlay_cache --game-id "$game_id" \
                      --cache-src-root "$cache_src_root" \
                      --stage "$payload" \
                      --cg-tag "$cg_tag"
psx_add_overlay_toolchain --stage "$payload" \
                          --recomp-dir "$(dirname -- "$recompiler_bin")" \
                          --recomp-tools "$fw/tools" \
                          --recomp-include "$fw/runtime/include" \
                          --dl-cache "$tools_dir" \
                          --platform linux
cp "$player_toml" "$payload/game.toml"
cp "$root/packaging/release/input.ini"      "$payload/input.ini"
cp "$root/packaging/release/START_HERE.txt" "$payload/START_HERE.txt"
cp "$root/LICENSE" "$root/README.md" "$payload/"

# recomp-ui resolves fonts/textures through SDL_GetBasePath(), which points at
# the real ELF inside the mount rather than psxrecomp's writable argv[0] anchor.
ln -s "../share/$PAYLOAD_DIR/assets" "$appdir/usr/bin/assets"

if command -v magick >/dev/null 2>&1; then image_tool=magick
elif command -v convert >/dev/null 2>&1; then image_tool=convert
else echo "ImageMagick is required for the AppImage icon." >&2; exit 1; fi
"$image_tool" "$root/$ICON_SOURCE" \
    -resize 240x240 -background transparent -gravity center -extent 256x256 \
    "$appdir/$DESKTOP_ID.png"
ln -s "$DESKTOP_ID.png" "$appdir/.DirIcon"

# --- pinned tooling --------------------------------------------------------
linuxdeploy_url=https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
linuxdeploy_sha=421ca71d5c69ea97c6309276232990d43df1dcece0edfaa26bbf926ff96ed12e
appimagetool_url=https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
appimagetool_sha=a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0

mkdir -p "$tools_dir"
fetch_tool() {
    url=$1; sha=$2; dest=$3
    if [ ! -f "$dest" ] || [ "$(sha256sum "$dest" | awk '{print $1}')" != "$sha" ]; then
        curl -fL --retry 3 "$url" -o "$dest.tmp"
        printf '%s  %s\n' "$sha" "$dest.tmp" | sha256sum -c -
        mv "$dest.tmp" "$dest"
    fi
    chmod 0755 "$dest"
}
linuxdeploy=$tools_dir/linuxdeploy-x86_64.AppImage
appimagetool=$tools_dir/appimagetool-x86_64.AppImage
fetch_tool "$linuxdeploy_url" "$linuxdeploy_sha" "$linuxdeploy"
fetch_tool "$appimagetool_url" "$appimagetool_sha" "$appimagetool"

export NO_STRIP=1
"$linuxdeploy" --appimage-extract-and-run \
    --appdir "$appdir" \
    --executable "$appdir/usr/bin/$EXE_NAME" \
    --desktop-file "$appdir/$DESKTOP_ID.desktop" \
    --icon-file "$appdir/$DESKTOP_ID.png"

# Normalise mtimes so the squashfs image is byte-stable across runs.
find "$appdir" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} + 2>/dev/null || true

rm -f -- "$output"
ARCH=x86_64 "$appimagetool" --appimage-extract-and-run "$appdir" "$output"
chmod 0755 "$output"

sha256sum "$output"
echo "AppImage: $output"
