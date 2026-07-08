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
Invoke-Native { cmake -S $Root -B $BuildPath -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=OFF -DPSX_LAUNCHER=ON } "cmake configure"
Invoke-Native { cmake --build $BuildPath -j $env:NUMBER_OF_PROCESSORS } "cmake build"

if (Test-Path $StageRoot) { Remove-Item -Recurse -Force $StageRoot }
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
