param(
    # Empty means "read packaging/release/VERSION", the single source of truth
    # shared with tools/package_appimage.sh so Windows and Linux releases do
    # not drift.
    [string]$Version = "",
    [string]$BuildDir = "build-release",
    [string]$RecompilerBuildDir = "recompiler/build",
    [int]$Jobs = 2,
    [switch]$SkipRegen,
    # Releases with an external runtime manifest require the exact validated
    # executable. Game-specific HLE inputs are maintained outside this repo.
    [string]$PrebuiltExecutable = "",
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
# Every release extracts original-disc members, builds native shards, and audits
# them through the framework method pipeline. Historical caches are not inputs.

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
$RuntimeManifestPath = Join-Path $PackagingRelease 'runtime.json'
if (Test-Path -LiteralPath $RuntimeManifestPath) {
    $RuntimeManifest = Get-Content -LiteralPath $RuntimeManifestPath -Raw | ConvertFrom-Json
    if ($RuntimeManifest.version -ne $Version) {
        throw "Release version differs from packaging/release/runtime.json"
    }
    if (-not $PrebuiltExecutable) {
        throw "This HLE release requires -PrebuiltExecutable from the validated external build. See packaging/release/runtime.json."
    }
    $PrebuiltExecutable = (Resolve-Path -LiteralPath $PrebuiltExecutable).Path
    if ((Get-FileHash -LiteralPath $PrebuiltExecutable -Algorithm SHA256).Hash -ne $RuntimeManifest.exe_sha256) {
        throw "Prebuilt executable does not match the validated release SHA-256"
    }
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
$RecompDir = if ([System.IO.Path]::IsPathRooted($RecompilerBuildDir)) {
    $RecompilerBuildDir
} else { Join-Path $FrameworkRoot $RecompilerBuildDir }
$RecompBin = Join-Path $RecompDir "psxrecomp-game.exe"
if (-not (Test-Path -LiteralPath (Join-Path $RecompDir "build.ninja"))) {
    Invoke-Native {
        & $CMake -S $RecompSourceDir -B $RecompDir -G Ninja -DCMAKE_BUILD_TYPE=Release
    } "recompiler configure"
}
Invoke-Native {
    & $CMake --build $RecompDir --target psxrecomp-game psxrecomp-bios -j $Jobs
} "recompiler build"

if (-not $SkipRegen) {
    Invoke-Native { & $RecompBin --config (Join-Path $Root 'game.toml') } 'base game regeneration'
}

# Build: Release, debug tools OFF, launcher ON. PSX_STATIC_RUNTIME defaults ON
# for MinGW Release so the exe imports only system DLLs (self-contained).
#
# This step also (re)writes psxrecomp-v4/runtime/include/overlay_codegen_hash.h
# via runtime.cmake's hash_codegen custom command, and the cache tag is derived
# from that header. So the order runtime build -> derive tag -> filter shards is
# load-bearing: derive the tag before this and it is computed from a header that
# does not exist yet or is stale, and every shard is filed under a namespace the
# shipped runtime does not scan.
if (-not $PrebuiltExecutable) {
    Invoke-Native { & $CMake -S $Root -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=OFF -DPSX_PGXP_VARIANT=OFF -DPSX_SDL_BACKEND=SDL3 "-DPSX_GAME_VERSION=$Version" `
        "-DPSXRECOMP_ROOT=$FrameworkRoot" "-DRECOMP_UI_ROOT=$RecompUiRoot" } "cmake configure"
    Invoke-Native { & $CMake --build $BuildPath --target psx-runtime -j $Jobs } "cmake build"
}

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
$DevExe = $PrebuiltExecutable
if (-not $DevExe) {
    $DevExe = Join-Path $BuildPath "ApeEscapeRecomp.exe"
    if (-not (Test-Path $DevExe)) { $DevExe = Join-Path $BuildPath "psx-runtime.exe" }
}
Copy-Item $DevExe (Join-Path $Stage "ApeEscapeRecomp.exe")
if (Test-Path -LiteralPath $RuntimeManifestPath) {
    Copy-Item -LiteralPath $RuntimeManifestPath -Destination (Join-Path $Stage 'RUNTIME_BUILD.json')
}
if (Test-Path (Join-Path $Root "README.md"))         { Copy-Item (Join-Path $Root "README.md") $Stage }
if (Test-Path (Join-Path $Root "LICENSE"))           { Copy-Item (Join-Path $Root "LICENSE") $Stage }
New-Item -ItemType Directory -Force (Join-Path $Stage 'docs') | Out-Null
Copy-Item (Join-Path $Root 'docs/AOT_OVERLAYS.md') (Join-Path $Stage 'docs')
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

# Fresh original-disc AOT is mandatory, including when base regeneration is skipped.
$RecompTools = (Resolve-Path -LiteralPath (Join-Path $FrameworkRoot "tools")).Path
$RecompInc = (Resolve-Path -LiteralPath (Join-Path $FrameworkRoot "runtime/include")).Path
$StagedGameToml = Join-Path $Stage 'game.toml'
$AotPython = Join-Path $MingwBin 'python.exe'
Invoke-Native {
    & $AotPython (Join-Path $RecompTools 'aot_overlay_pipeline.py') release `
        --profile (Join-Path $Root 'aot/overlays.json') `
        --game-toml (Join-Path $Root 'game.toml') --runtime-config $StagedGameToml `
        --runtime-build-dir $BuildPath --runtime-target psx-runtime `
        --recompiler $RecompBin --work-dir (Join-Path $Root 'build-aot') `
        --stage $Stage --gcc (Join-Path $MingwBin 'gcc.exe') --workers $Jobs
} 'original-disc AOT extraction, compilation and audit'
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
                "dinput8.dll","rpcrt4.dll","hid.dll","cfgmgr32.dll","opengl32.dll",
                "ntdll.dll","bcrypt.dll","dwmapi.dll","shlwapi.dll","ucrtbase.dll",
                "d2d1.dll","dwrite.dll")
$nonSystem = $imports | Where-Object {
    $systemDlls -notcontains $_.ToLower() -and $_ -notmatch '^api-ms-win-crt-[a-z0-9-]+\.dll$'
}
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

The package includes native shards for 47 archive overlays and two minigame
executables, extracted from the original disc. Fallback remains enabled.
See docs/AOT_OVERLAYS.md and AOT_CACHE_AUDIT.json for coverage and limits.

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
# Keep framework/UI notices and source pins with the playable package.
$Licenses = Join-Path $Stage 'licenses'
New-Item -ItemType Directory -Path $Licenses -Force | Out-Null
Copy-Item -Path (Join-Path $FrameworkRoot 'runtime/licenses/*') -Destination $Licenses -Force
foreach ($notice in @('LICENSE', 'THIRD_PARTY_ATTRIBUTION.md')) {
    $source = Join-Path $FrameworkRoot $notice
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $Licenses "psxrecomp-$notice") -Force
    }
}
$UiLicense = Join-Path $Root 'recomp-ui/LICENSE'
if (Test-Path -LiteralPath $UiLicense) {
    Copy-Item -LiteralPath $UiLicense -Destination (Join-Path $Licenses 'recomp-ui-LICENSE') -Force
}
foreach ($doc in @('VERSION','RELEASE_NOTES.md','framework_pins.txt','BUILD_PROVENANCE.json')) {
    $source = Join-Path $Root $doc
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $Stage -Force }
}
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath -Force

Write-Host "Wrote $ZipPath"
