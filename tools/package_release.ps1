param(
    # Empty means "read packaging/release/VERSION", the single source of truth
    # shared with tools/package_appimage.sh so Windows and Linux releases do
    # not drift.
    [string]$Version = "",
    [string]$BuildDir = "build-release",
    # Where the accumulated overlay shard cache lives: the --out-dir
    # compile_overlays.py writes to, which is also where the runtime's own
    # autocompile deposits shards (<exe>/cache). Only shards under THIS
    # release's codegen tag are staged; see the staging block near the end.
    #
    # There is deliberately no switch to release without one. game.toml
    # declares [runtime] overlay_cache = true, so a cache-less package is a
    # package that promised native overlays and shipped the interpreter.
    [string]$CacheBuildDir = "build-release",
    # Framework and launcher checkouts to build and stage from. Empty means the
    # in-repo psxrecomp-v4 / recomp-ui submodules (normally directory junctions
    # to the shared checkouts). Override either one to validate a release
    # against a worktree WITHOUT moving a submodule pin -- the same
    # -DPSXRECOMP_ROOT / -DRECOMP_UI_ROOT pattern an ordinary validation build
    # uses. Without this the packager could only ever be exercised against
    # whatever the submodules happen to point at, which is precisely how a
    # framework-side layout change reaches a release packager untested.
    [string]$FrameworkDir = "",
    [string]$RecompUiDir = ""
)

# Ape Escape (SCUS-94423) release packager. Adapted from MegaManX6Recomp.
#
# Overlay shard releases are expected to share capture evidence across
# platforms: Windows and Linux shards must be rebuilt for their own loader
# format, but both should come from the same validated overlay_captures.json
# manifest so coverage does not drift between the zip and AppImage.
#
# NOTE: this intentionally does NOT regenerate the game C. v0.0.1 ships the
# exact recompiled code that was validated booting to 3D title/gameplay; the
# merged-master recompiler's wider function discovery is proven on Tomba/MMX6
# but Ape's generated/ has not been re-validated against it, so we build the
# validated generated/ as-is. (Re-add a regen step here once Ape is revalidated
# against the new discovery.)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$PackagingRelease = Join-Path $Root "packaging\release"
if (-not $Version) {
    $VersionFile = Join-Path $PackagingRelease "VERSION"
    if (-not (Test-Path -LiteralPath $VersionFile)) {
        throw "No -Version given and $VersionFile is missing"
    }
    $Version = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
    if (-not $Version) { throw "$VersionFile is empty" }
}
if ($FrameworkDir) {
    $FrameworkRoot = (Resolve-Path -LiteralPath $FrameworkDir).Path
} else {
    $FrameworkRoot = Join-Path $Root "psxrecomp-v4"
}
if (-not (Test-Path -LiteralPath (Join-Path $FrameworkRoot "tools\release_overlay_stage.ps1"))) {
    throw ("No psxrecomp framework checkout at $FrameworkRoot " +
           "(expected tools\release_overlay_stage.ps1). Run " +
           "'git submodule update --init psxrecomp-v4', or pass " +
           "-FrameworkDir <path-to-psxrecomp>.")
}
if ($RecompUiDir) {
    $RecompUiRoot = (Resolve-Path -LiteralPath $RecompUiDir).Path
} else {
    $RecompUiRoot = Join-Path $Root "recomp-ui"
}
$BuildPath = Join-Path $Root $BuildDir
$StageRoot = Join-Path $Root "release-stage"
$Stage = Join-Path $StageRoot "ApeEscapeRecomp-windows-x64"
$ZipPath = Join-Path $Root ("ApeEscapeRecomp-{0}-windows-x64.zip" -f $Version)
$MingwBin = "C:\msys64\mingw64\bin"
$CMake = Join-Path $MingwBin "cmake.exe"

$env:PATH = "$MingwBin;$env:PATH"

# cmake writes benign warnings (e.g. freetype's cmake_minimum_required
# deprecation) to STDERR. Under $ErrorActionPreference='Stop', PowerShell 5.1
# wraps native-command stderr as a terminating error and would abort the whole
# release for a non-error. Run the native cmake invocations with the preference
# relaxed and gate on the real signal -- $LASTEXITCODE -- instead.
function Invoke-Native {
    param([scriptblock]$Cmd, [string]$What)
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Cmd
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    if ($code -ne 0) { throw "$What failed (exit $code)" }
}

# One scalar out of one table of a game.toml, or $null.
#
# Used below against the STAGED game.toml, never the dev one. The cache tag
# folds in a hash of the config file, so the dev config and the player config
# name DIFFERENT cache namespaces; deciding "does this release want a cache" or
# "which game id keys the cache dir" from the dev config would let the packager
# satisfy a promise the shipped exe never makes (or miss one it does).
#
# Not a TOML parser on purpose: this needs to work with nothing but Windows
# PowerShell 5.1, and it only ever reads two scalars. Comment lines are skipped
# because game.toml carries commented-out examples of the very keys read here.
function Get-TomlScalar {
    param(
        [Parameter(Mandatory)][string]$GameToml,
        [Parameter(Mandatory)][string]$Table,
        [Parameter(Mandatory)][string]$Key
    )
    $section = ""
    foreach ($raw in (Get-Content -LiteralPath $GameToml)) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith("#")) { continue }
        if ($line -match '^\[\[?([^\]]+)\]\]?$') { $section = $Matches[1].Trim(); continue }
        if ($section -ne $Table) { continue }
        if ($line -match ('^' + [regex]::Escape($Key) + '\s*=\s*(.+?)\s*(?:#.*)?$')) {
            return $Matches[1].Trim().Trim('"').Trim("'")
        }
    }
    return $null
}

# ---- Recompiler ----------------------------------------------------------
# Needed by two things below, both of them new here: Get-OverlayCgTag asks this
# binary for the canonical hash of the config fields that define a cache
# namespace (--overlay-config-hash), and Add-OverlayToolchain ships it so a
# player with no compiler can still turn captured overlays into native code.
#
# BUILD it rather than trusting whatever is in the build dir. The recompiler
# bakes the emitter-source hash at ITS build time while the cache tag reads the
# same hash out of runtime/include/overlay_codegen_hash.h, and nothing else ties
# the two together: a recompiler built before the last emitter change emits OLD
# code stamped with the CURRENT tag -- read tag == write tag, content stale.
# compile_overlays.verify_recompiler_matches_tag() refuses to build shards in
# that state, so a stale binary here fails the cache build later with a message
# about a mismatch instead of here with a build.
$RecompSourceDir = Join-Path $FrameworkRoot "recompiler"
$RecompDir = Join-Path $RecompSourceDir "build"
$RecompBin = Join-Path $RecompDir "psxrecomp-game.exe"
if (-not (Test-Path -LiteralPath (Join-Path $RecompDir "build.ninja"))) {
    Invoke-Native {
        & $CMake -S $RecompSourceDir -B $RecompDir -G Ninja -DCMAKE_BUILD_TYPE=Release
    } "recompiler configure"
}
Invoke-Native {
    & $CMake --build $RecompDir --target psxrecomp-game -j $env:NUMBER_OF_PROCESSORS
} "recompiler build"

# Build: Release, debug tools OFF, launcher ON. PSX_STATIC_RUNTIME defaults ON
# for MinGW Release so the exe imports only system DLLs (self-contained).
#
# This step also (re)writes psxrecomp-v4/runtime/include/overlay_codegen_hash.h
# via runtime.cmake's hash_codegen custom command, and the cache tag is derived
# from that header. So the order runtime build -> derive tag -> filter shards is
# load-bearing: derive the tag before this and it is computed from a header that
# does not exist yet or is stale, and every shard is filed under a namespace the
# shipped runtime does not scan.
Invoke-Native { & $CMake -S $Root -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=OFF `
    "-DPSXRECOMP_ROOT=$FrameworkRoot" "-DRECOMP_UI_ROOT=$RecompUiRoot" } "cmake configure"
Invoke-Native { & $CMake --build $BuildPath -j $env:NUMBER_OF_PROCESSORS } "cmake build"

if (Test-Path $StageRoot) {
    $resolvedRoot = (Resolve-Path $Root).Path.TrimEnd('\')
    $resolvedStage = (Resolve-Path $StageRoot).Path.TrimEnd('\')
    if (-not $resolvedStage.StartsWith($resolvedRoot + "\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete stage path outside repo root: $resolvedStage"
    }
    Remove-Item -LiteralPath $StageRoot -Recurse -Force
}
New-Item -ItemType Directory -Force $Stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Stage "saves") | Out-Null

# The runtime target's OUTPUT_NAME is derived from window_title -> the built exe
# is ApeEscapeRecomp.exe, NOT psx-runtime.exe. Prefer that (fall back to the
# generic name for older builds). Copying psx-runtime.exe shipped a STALE binary.
$DevExe = Join-Path $BuildPath "ApeEscapeRecomp.exe"
if (-not (Test-Path $DevExe)) { $DevExe = Join-Path $BuildPath "psx-runtime.exe" }
Copy-Item $DevExe (Join-Path $Stage "ApeEscapeRecomp.exe")
if (Test-Path (Join-Path $Root "README.md"))         { Copy-Item (Join-Path $Root "README.md") $Stage }
if (Test-Path (Join-Path $Root "LICENSE"))           { Copy-Item (Join-Path $Root "LICENSE") $Stage }
$BundledBiosSrc = Join-Path $BuildPath "bios"
if (!(Test-Path (Join-Path $BundledBiosSrc "openbios.bin")) -or
    (Get-Item (Join-Path $BundledBiosSrc "openbios.bin")).Length -ne 524288 -or
    !(Test-Path (Join-Path $BundledBiosSrc "OpenBIOS.LICENSE"))) {
    throw "Runtime build did not stage OpenBIOS and its MIT notice"
}
$BundledBiosDst = Join-Path $Stage "bios"
New-Item -ItemType Directory -Force $BundledBiosDst | Out-Null
Copy-Item (Join-Path $BundledBiosSrc "openbios.bin") $BundledBiosDst
Copy-Item (Join-Path $BundledBiosSrc "OpenBIOS.LICENSE") $BundledBiosDst
if (Test-Path (Join-Path $Root "RELEASE_NOTES.md"))  { Copy-Item (Join-Path $Root "RELEASE_NOTES.md") $Stage }

# Launcher assets: this build ships the shared recomp-ui Dear ImGui launcher
# (RECOMP_LAUNCHER; see main.cpp + recomp-ui/recomp_ui.cmake), which loads from
# <exe>/assets/ (fonts + img TGAs) staged next to the exe by
# recomp_target_launcher_ui's POST_BUILD.
$AssetsSrc = Join-Path $BuildPath "assets"
if (-not (Test-Path (Join-Path $AssetsSrc "img"))) {
    throw "recomp-ui launcher assets missing at $AssetsSrc -- was the recomp-ui launcher built (recomp-ui junction present)?"
}
Copy-Item -Recurse -Force $AssetsSrc (Join-Path $Stage "assets")
$fontCount = (Get-ChildItem (Join-Path $Stage "assets/fonts") -Filter *.ttf -ErrorAction SilentlyContinue).Count
$imgCount  = (Get-ChildItem (Join-Path $Stage "assets/img")   -Filter *.tga -ErrorAction SilentlyContinue).Count
Write-Host "Bundled recomp-ui launcher assets: $fontCount font(s) + $imgCount image(s)"

# Built-in mod catalog, staged into <build>/mods/bundled by the runtime
# target's POST_BUILD command (psxrecomp_add_runtime_target's
# PRELOADED_MODS_DIR stages the framework's mods/builtin/packages and this
# repo's mods/preloaded/packages there together).
#
# Routed through the framework's shared Add-ModCatalog instead of the hand-
# written block that used to live here. That block hard-coded "exactly 4 ape.*
# manifests" and globbed mods/packages, and both halves of it were wrong:
#
#   * the count went stale by construction -- it describes only this title's
#     half of a catalog the framework also contributes to, so it said nothing
#     about whether the shared psx.* packages shipped at all (the same class of
#     assertion that made Tomba 2 unreleasable on 2026-09-01 when the framework
#     gained a fifth builtin);
#   * mods/packages is the PRE-SPLIT layout. Framework 4cc04be3 moved staged
#     build output to mods/bundled, and nothing in this repo followed, so at
#     framework master the four ape.* packages were staged where neither the
#     launcher nor a packager reads them (bead beads-eio.3.101).
#
# Add-ModCatalog asserts the invariant instead of a number: every package the
# SOURCES define -- this repo's mods/preloaded/packages and the framework's
# mods/builtin/packages -- must survive into the staged catalog. That cannot go
# stale when a mod is added on either side, and it still catches the failure
# that matters, a mod silently not shipping. It also strips the two things
# under mods/ that belong to this machine (installed/ and state.toml).
. (Join-Path $FrameworkRoot "tools\release_overlay_stage.ps1")
Add-ModCatalog -BuildPath $BuildPath -Stage $Stage `
               -GameModSource (Join-Path $Root "mods\preloaded") `
               -FrameworkModSource (Join-Path $FrameworkRoot "mods\builtin") | Out-Null

# Player-facing game.toml: copy the REAL game.toml (the single source of truth
# for all runtime/video/controller/widescreen config) minus the dev-only [audit]
# section, so the shipped config can never drift from what was validated.
$realToml = Get-Content (Join-Path $Root "game.toml") -Raw
# Cut at the dev-only audit block. Match the ASCII word "Audit-specific" (its
# comment line uses non-ASCII box-drawing chars we must not embed here), then
# back up to that line's start so the comment goes too; fall back to [audit].
$idx = $realToml.IndexOf("Audit-specific")
if ($idx -ge 0) {
    $ls = $realToml.LastIndexOf("`n", $idx)
    $cut = if ($ls -ge 0) { $ls } else { 0 }
} else {
    $cut = $realToml.IndexOf("[audit]")
}
$playerToml = if ($cut -ge 0) { $realToml.Substring(0, $cut).TrimEnd() + "`n" } else { $realToml }
$playerToml | Set-Content -Encoding ASCII (Join-Path $Stage "game.toml")
Write-Host "Staged player game.toml from real game.toml (audit section stripped)"

# ---- Overlay shard cache + self-contained overlay toolchain --------------
# BOTH staged by the framework's SHARED module, psxrecomp-v4/tools/
# release_overlay_stage.ps1, dot-sourced above for Add-ModCatalog. This is a
# CALL and must stay one: hand-copying this logic is precisely how the ecosystem
# diverged. Measured across the five titles' packagers on 2026-09-02:
#
#   * THIS packager staged NEITHER. Ape Escape's packager was created on
#     2026-07-05 by copying MegaManX6's and trimming it from 345 lines to 146,
#     and the overlay cache and toolchain staging were among the lines cut. No
#     Ape commit has ever contained the string "overlay_toolchain", so every Ape
#     release ever published ran 100% of its overlay dispatches on the dirty-RAM
#     interpreter (disp_native=0, disp_interp=4,480,307 in a single session) --
#     while game.toml's own comment above overlay_cache promised "the play-free
#     AOT overlay cache beside the executable ... as a fail-safe". That comment
#     was aspirational for this title's entire life. It is true as of this
#     commit.
#   * MegaManX4, X5 and X6 each carried their own ~60-line copy instead. All
#     three rebuilt the cache tag from a local PowerShell format string that
#     predated fields the real tag has since grown (_gc<config-hash>,
#     _f<flavor>), so their filters matched nothing and a perfectly good cache
#     staged ZERO shards; all three also carried local toolchain download code
#     instead of the framework's pinned, hash-checked staging path.
#
# THE RULE, and why it is this rule and not a per-title list.
#
# The STAGED game.toml is the contract the shipped exe reads. When it declares
# [runtime] overlay_cache = true, the runtime scans
# <exe>/cache/<id>/<compiler>/<arch-abi>/<tag>/ on every launch (main.cpp
# deferred_overlay_cache -> overlay_loader_init), so a package that declares it
# and ships nothing there has promised native overlays and delivered the
# interpreter. Add-OverlayCache therefore THROWS. This project has already
# shipped incomplete packages because a warning scrolled past in a build log;
# this path stays fail-loud.
#
# If a title's staged config does NOT declare overlay_cache, no cache is staged,
# that is said out loud, and we assert none rode along anyway -- shards the
# runtime never scans are pure download weight. The predicate is the shipped
# config either way, so the rule is derived from the release rather than
# maintained as a list of exceptions.
#
# The TOOLCHAIN is staged UNCONDITIONALLY in both branches. The runtime gates
# autocompile on exactly <exe>/overlay_toolchain/python/python.exe (main.cpp
# `tk_present`) and synthesises the whole compile command out of that directory
# when the staged config carries no overlay_autocompile_cmd -- which every
# release config omits, because the dev command points at psxrecomp-v4/ paths
# that do not ship. Ape's config carries no autocompile command at all, so for
# THIS title the bundled toolchain is the only path from a captured overlay to
# native code that exists. Without it the capture -> compile fail-safe can never
# fire for any title, cache or no cache, and there is no reason to withhold it.
$RecompTools = (Resolve-Path -LiteralPath (Join-Path $FrameworkRoot "tools")).Path
$RecompInc   = (Resolve-Path -LiteralPath (Join-Path $FrameworkRoot "runtime\include")).Path
$StagedGameToml = Join-Path $Stage "game.toml"

# The loader keys the cache directory by the [game] id in the config it loaded,
# so read the id out of the STAGED config rather than repeating "SCUS-94423"
# here. A literal would be one more thing that can disagree with the shipped
# file, and the disagreement would be silent: shards under the wrong id are
# simply never scanned.
$CacheGameId = Get-TomlScalar -GameToml $StagedGameToml -Table "game" -Key "id"
if (-not $CacheGameId) {
    throw "Could not read [game] id from the staged config $StagedGameToml"
}

# Cache SOURCE root (the parent of the per-game directory; the module appends
# the game id). NORMALISE before the module slices relative paths out of it:
# -CacheBuildDir may legitimately contain '..' for a cache kept outside the
# repo, Join-Path does not collapse that, and the unresolved string is then
# LONGER than the real prefix -- which once made every staged shard land at
# cache/<id>/<truncated garbage>/ while the packager still reported a healthy
# shard count.
$CacheSrcRoot = if ([System.IO.Path]::IsPathRooted($CacheBuildDir)) {
    $CacheBuildDir
} else {
    Join-Path $Root $CacheBuildDir
}
$CacheSrcRootRaw = $CacheSrcRoot
if (Test-Path -LiteralPath $CacheSrcRoot) {
    $CacheSrcRoot = (Resolve-Path -LiteralPath $CacheSrcRoot).Path
}
$CacheSrcRoot = Join-Path $CacheSrcRoot "cache"
# Quarantined caches are never an input. A matching cg tag does NOT prove
# compatibility: a quarantined cross-version Ape cache carries the SAME tag as a
# good one (measured: cg10_a4319b6f_gcc31ae4a9_f0 on both, with only 6 of 50
# shard filenames in common, and only the quarantined copy carrying an
# .abi_00000015.ok memo for an ABI the current overlay_api.h has moved past). So
# the tag filter cannot reject it and the PATH is the only signal left. Check
# the given path and its resolved form -- a junction can point a clean-looking
# name at a quarantined tree.
foreach ($p in @($CacheSrcRootRaw, $CacheSrcRoot)) {
    if ($p -match 'QUARANTINE') {
        throw ("Refusing a quarantined overlay cache source: $p. A quarantined " +
               "cross-version cache can carry the same cg tag as a good one, so " +
               "the tag filter cannot reject it; point -CacheBuildDir at a cache " +
               "built by this release's own toolchain.")
    }
}

# The tag comes from compile_overlays.cache_tag() via the module, never from a
# format string here, and it is derived from the STAGED config for the reason
# above. It is derived HERE, after the runtime build wrote
# runtime/include/overlay_codegen_hash.h and after the staged game.toml exists.
$CgTag = Get-OverlayCgTag -RecompTools $RecompTools -RecompInc $RecompInc `
                          -GameExe $RecompBin -GameToml $StagedGameToml
Write-Host "Release codegen tag: $CgTag (only this cache namespace is shipped)"

$OverlayCacheDeclared =
    ((Get-TomlScalar -GameToml $StagedGameToml -Table "runtime" -Key "overlay_cache") -eq "true")
if ($OverlayCacheDeclared) {
    Write-Host ("Staged game.toml declares [runtime] overlay_cache = true -- a " +
                "shard cache for $CgTag is REQUIRED in this package")
    Add-OverlayCache -GameId $CacheGameId -CacheSrcRoot $CacheSrcRoot `
                     -Stage $Stage -CgTag $CgTag | Out-Null
} else {
    Write-Host ("Staged game.toml does not declare [runtime] overlay_cache -- " +
                "staging NO shard cache; the shipped runtime would never scan one")
    if (Test-Path -LiteralPath (Join-Path $Stage "cache")) {
        throw ("The staged config does not declare [runtime] overlay_cache, yet " +
               "a cache/ tree is present in the stage. The runtime never scans " +
               "it, so it is download weight that also implies a guarantee the " +
               "package does not make.")
    }
}
Add-OverlayToolchain -Stage $Stage -RecompDir $RecompDir -RecompTools $RecompTools `
                     -RecompInc $RecompInc -MingwBin $MingwBin `
                     -DlCache (Join-Path $Root "tools\_toolchain_cache") | Out-Null


# Verify self-containment: imports must be system DLLs only.
$objdump = Join-Path $MingwBin "objdump.exe"
$imports = & $objdump -p (Join-Path $Stage "ApeEscapeRecomp.exe") |
    Select-String "DLL Name: (.+)" | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
$systemDlls = @("kernel32.dll","user32.dll","gdi32.dll","shell32.dll","msvcrt.dll",
                "advapi32.dll","ws2_32.dll","comdlg32.dll","dbghelp.dll","ole32.dll",
                "oleaut32.dll","winmm.dll","imm32.dll","version.dll","setupapi.dll",
                "dinput8.dll","rpcrt4.dll","hid.dll","cfgmgr32.dll","opengl32.dll")
$nonSystem = $imports | Where-Object { $systemDlls -notcontains $_.ToLower() }
if ($nonSystem) {
    throw "Release exe is NOT self-contained -- imports non-system DLL(s): $($nonSystem -join ', ')"
}
Write-Host "Verified self-contained: imports only system DLLs ($($imports.Count) total)"

@"
ApeEscapeRecomp $Version

Ape Escape boots from the PlayStation BIOS and plays into its 3D title and
gameplay. This is an in-development preview; a full playthrough has not been
verified, so expect rough edges.

This package includes the MIT-licensed OpenBIOS from PCSX-Redux and its notice
in bios/OpenBIOS.LICENSE. It does not include the Ape Escape disc, a retail
PlayStation BIOS, save data, or game assets.

First launch:
1. Run ApeEscapeRecomp.exe. A launcher window opens.
2. OpenBIOS is selected automatically. You may optionally select your legally
   obtained SCPH1001.BIN in the BIOS row.
3. Set the game disc: your legally obtained Ape Escape (USA, SCUS-94423) image.
4. Adjust options and choose any features on the Mods page, then press
   Launch. Ape-specific bundled mods include Widescreen, Frame Smoothing, Skip
   FMVs, and Quick Gadget Select. Frame Smoothing is temporal blending, not
   motion-vector frame generation. Quick Gadget Select was contributed by mthsk.

Ape Escape requires an analog (DualShock) controller -- a controller is
strongly recommended. The selected BIOS/disc paths are saved next to the exe.

Disc image formats: .cue + .bin (pick the .cue) or .bin. Do NOT convert to a
2048-byte "cooked" .iso -- it discards the XA sectors used for FMV/audio.

Save states and rewind are available through the launcher hotkeys. Defaults:
F7 opens the save-state menu and F8 rewinds.

Memory cards, save states, and rewind data are stored in the saves directory.
"@ | Set-Content -Encoding ASCII (Join-Path $Stage "START_HERE.txt")

if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath -Force

Write-Host "Wrote $ZipPath"
