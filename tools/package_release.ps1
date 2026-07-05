param(
    [string]$Version = "v0.0.1",
    [string]$BuildDir = "build-release"
)

# Ape Escape (SCUS-94423) release packager. Adapted from MegaManX6Recomp.
#
# NOTE: this intentionally does NOT regenerate the game C. v0.0.1 ships the
# exact recompiled code that was validated booting to 3D title/gameplay; the
# merged-master recompiler's wider function discovery is proven on Tomba/MMX6
# but Ape's generated/ has not been re-validated against it, so we build the
# validated generated/ as-is. (Re-add a regen step here once Ape is revalidated
# against the new discovery.)

$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$BuildPath = Join-Path $Root $BuildDir
$StageRoot = Join-Path $Root "release-stage"
$Stage = Join-Path $StageRoot "ApeEscapeRecomp-windows-x64"
$ZipPath = Join-Path $Root ("ApeEscapeRecomp-{0}-windows-x64.zip" -f $Version)
$MingwBin = "C:\msys64\mingw64\bin"

$env:PATH = "$MingwBin;$env:PATH"

# Build: Release, debug tools OFF, launcher ON. PSX_STATIC_RUNTIME defaults ON
# for MinGW Release so the exe imports only system DLLs (self-contained).
cmake -S $Root -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=OFF -DPSX_LAUNCHER=ON
cmake --build $BuildPath -j $env:NUMBER_OF_PROCESSORS

if (Test-Path $StageRoot) { Remove-Item -Recurse -Force $StageRoot }
New-Item -ItemType Directory -Force $Stage | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Stage "saves") | Out-Null

Copy-Item (Join-Path $BuildPath "psx-runtime.exe") (Join-Path $Stage "ApeEscapeRecomp.exe")
if (Test-Path (Join-Path $Root "README.md"))         { Copy-Item (Join-Path $Root "README.md") $Stage }
if (Test-Path (Join-Path $Root "LICENSE"))           { Copy-Item (Join-Path $Root "LICENSE") $Stage }
if (Test-Path (Join-Path $Root "RELEASE_NOTES.md"))  { Copy-Item (Join-Path $Root "RELEASE_NOTES.md") $Stage }

# Launcher assets (staged next to the exe by the PSX_LAUNCHER build).
$LauncherRml = Join-Path $BuildPath "launcher.rml"
if (-not (Test-Path $LauncherRml)) {
    throw "Launcher assets missing at $BuildPath (no launcher.rml) -- was -DPSX_LAUNCHER=ON honored?"
}
Copy-Item $LauncherRml $Stage
foreach ($dir in @("fonts","img")) {
    $src = Join-Path $BuildPath $dir
    if (-not (Test-Path $src)) { throw "Launcher asset dir missing: $src" }
    Copy-Item -Recurse -Force $src (Join-Path $Stage $dir)
}
$fontCount = (Get-ChildItem (Join-Path $Stage "fonts") -Filter *.ttf -ErrorAction SilentlyContinue).Count
$imgCount  = (Get-ChildItem (Join-Path $Stage "img") -Filter *.png -ErrorAction SilentlyContinue).Count
Write-Host "Bundled launcher assets: launcher.rml + $fontCount font(s) + $imgCount image(s)"

# Player-facing game.toml (dev-only [audit] section omitted).
@"
[game]
name = "Ape Escape"
id = "SCUS-94423"
exe = "apeescape/SCUS_944.23"
disc = "apeescape/Ape Escape (USA).cue"
load_address = "0x80010000"
entry_pc = "0x800A3660"
text_size = "0x000A5000"
stack_base = "0x801FFFF0"

# Required block; used only by the developer recompiler tool, not at runtime.
[recompiler]
seeds = "seeds/ghidra_funcs.txt"
out_dir = "generated"

# ---- Player-adjustable options ------------------------------------------
[runtime]
window_title = "Ape Escape Recompiled"
memcard_dir = "saves"
# Authentic 1x CD timing; fast loads come from turbo_loads (preserves timing).
disc_speed = "1x"
fast_boot  = false
# Turbo loads: run the machine at full host speed during a load (timing
# preserved, audio plays through). Toggleable in the launcher (Settings).
turbo_loads = true

[video]
# supersampling: render at N* native res and downsample (1 = native, 2 = good).
supersampling = 2
antialiasing  = true
texture_filtering = "nearest"
renderer = "opengl"
# Skip full-motion videos automatically (default on). Launcher: Settings -> Skip FMVs.
auto_skip_fmv = true
aspect_ratio = "4:3"

# ---- Controller ---------------------------------------------------------
# Ape Escape requires a DualShock (it won't poll the face buttons until it
# sees an analog pad), so present an analog pad by default.
[controller]
default_analog = true
deadzone = 12000
"@ | Set-Content -Encoding ASCII (Join-Path $Stage "game.toml")

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
gameplay. This is a very early first preview (v0.0.1) -- a full playthrough has
not been verified, so expect rough edges.

This package does not include the Ape Escape disc, the PlayStation BIOS, or any
game assets -- you supply those from your own collection, and ApeEscapeRecomp
asks for them one at a time. The executable contains a statically recompiled
(machine-translated) build of the game's code, the same distribution model used
by other static recompilation projects such as N64: Recompiled.

First launch:
1. Run ApeEscapeRecomp.exe. A launcher window opens.
2. Set your PlayStation BIOS: your legally obtained SCPH1001.BIN (512 KB).
3. Set the game disc: your legally obtained Ape Escape (USA, SCUS-94423) image.
4. Adjust options if you like, then press Launch.

Ape Escape requires an analog (DualShock) controller -- a controller is
strongly recommended. The selected BIOS/disc paths are saved next to the exe.

Disc image formats: .cue + .bin (pick the .cue) or .bin. Do NOT convert to a
2048-byte "cooked" .iso -- it discards the XA sectors used for FMV/audio.

Memory cards are stored in the saves directory.
"@ | Set-Content -Encoding ASCII (Join-Path $Stage "START_HERE.txt")

if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $ZipPath -Force

Write-Host "Wrote $ZipPath"
