param(
    [string]$Python = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe",
    [string]$Recompiler = "",
    [string]$Gcc = "C:\msys64\mingw64\bin\gcc.exe",
    [int]$Jobs = 6,
    [switch]$SkipRuntimeBuild
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$frameworkRoot = (Resolve-Path (Join-Path $projectRoot "psxrecomp-v4")).Path
$buildDir = Join-Path $projectRoot "build-aot"
$captures = Join-Path $buildDir "playfree_captures.json"
$gameToml = Join-Path $projectRoot "game.toml"
$runtimeInclude = Join-Path $frameworkRoot "runtime\include"

if (-not $Recompiler) {
    $Recompiler = Join-Path $frameworkRoot "recompiler\build\psxrecomp-game.exe"
}

foreach ($required in @($Python, $Recompiler, $Gcc, $gameToml, $runtimeInclude)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required AOT input is missing: $required"
    }
}

# Builds and helper tools stay unobtrusive. Child processes inherit this class.
(Get-Process -Id $PID).PriorityClass = "BelowNormal"
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

function Invoke-Checked {
    param([scriptblock]$Command, [string]$Label)
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

$extractor = Join-Path $frameworkRoot "tools\aot_overlay_spike\extract_generic.py"
$compiler = Join-Path $frameworkRoot "tools\compile_overlays.py"
$reporter = Join-Path $frameworkRoot "tools\aot_overlay_spike\coverage_report.py"
$biosDispatch = Join-Path $frameworkRoot "generated\SCPH1001_dispatch.c"

Invoke-Checked {
    & $Python $extractor --game-toml $gameToml --recompiler $Recompiler `
        --out $captures
} "play-free extraction"

Invoke-Checked {
    & $Python $compiler --captures $captures --game-toml $gameToml `
        --recompiler $Recompiler --runtime-include $runtimeInclude --gcc $Gcc `
        --out-dir (Join-Path $buildDir "cache") --jobs $Jobs --cps
} "overlay shard compilation"

if (-not $SkipRuntimeBuild) {
    Invoke-Checked {
        & cmake -S $projectRoot -B $buildDir -G Ninja `
            -DCMAKE_BUILD_TYPE=RelWithDebInfo `
            "-DPSXRECOMP_ROOT=$frameworkRoot" `
            -DPSX_DEBUG_TOOLS=ON -DPSX_LAUNCHER=OFF -DPSX_ENABLE_VULKAN=OFF
    } "runtime configure"
    Invoke-Checked {
        & cmake --build $buildDir --target psx-runtime -j $Jobs
    } "runtime build"
}

$liveCaptures = Join-Path $buildDir "overlay_captures.json"
if (Test-Path -LiteralPath $liveCaptures) {
    $reportDir = Join-Path $projectRoot "docs\aot_coverage"
    New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
    $codegenHash = (& $Recompiler --codegen-hash).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $codegenHash) {
        throw "Could not query the recompiler codegen hash"
    }
    $abiRoot = Join-Path $buildDir "cache\SCUS-94423\gcc\win-x64"
    $staticDirs = @(
        Get-ChildItem -LiteralPath $abiRoot -Directory -ErrorAction Stop |
            Where-Object { $_.Name -like "cg*_*$codegenHash" }
    )
    if ($staticDirs.Count -ne 1) {
        throw "Expected one current codegen cache below $abiRoot; found $($staticDirs.Count)"
    }
    $reportArgs = @(
        $reporter,
        "--static", $staticDirs[0].FullName,
        "--bios-dispatch", $biosDispatch,
        "--captures", $liveCaptures,
        "--game", "SCUS-94423",
        "--out-md", (Join-Path $reportDir "SCUS-94423_gaps.md"),
        "--out-json", (Join-Path $reportDir "SCUS-94423_gaps.json")
    )
    $addendum = Join-Path $buildDir "overlay_captures.addendum.jsonl"
    if (Test-Path -LiteralPath $addendum) {
        $reportArgs += @("--addendum", $addendum)
    }
    Invoke-Checked { & $Python @reportArgs } "coverage scoreboard"
}

Write-Host "Ape AOT pipeline complete: $buildDir"
