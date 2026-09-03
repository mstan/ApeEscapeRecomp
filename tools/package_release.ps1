param(
    # Empty means "read packaging/release/VERSION", the single source of truth
    # shared with tools/package_appimage.sh so Windows and Linux releases do
    # not drift.
    [string]$Version = "",
    [string]$BuildDir = "build-release",
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

# Build: Release, debug tools OFF, launcher ON. PSX_STATIC_RUNTIME defaults ON
# for MinGW Release so the exe imports only system DLLs (self-contained).
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
