param(
    [string]$PSXRECOMP_ROOT = $env:PSXRECOMP_ROOT
)
$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($PSXRECOMP_ROOT)) {
    $PSXRECOMP_ROOT = Join-Path $Root 'psxrecomp-v4'
}
$Framework = (Resolve-Path $PSXRECOMP_ROOT).Path
$Config    = Join-Path $Root 'game.toml'
$ToolCandidates = @(
    (Join-Path $Framework 'recompiler/build/psxrecomp-game.exe'),
    (Join-Path $Framework 'recompiler/build-codex3/psxrecomp-game.exe'),
    (Join-Path $Framework 'recompiler/build-codex/psxrecomp-game.exe')
)
$Tool = $ToolCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (!$Tool) {
    throw "psxrecomp-game not built under $Framework/recompiler"
}
if (!(Test-Path $Config)) {
    throw "game.toml not found: $Config"
}

# Config-driven invocation. The TOML describes exe, seeds, out_dir —
# see psxrecomp-v4/docs/config_schema.md.
Push-Location $Root
try {
    & $Tool --config $Config
    if ($LASTEXITCODE -ne 0) {
        throw "psxrecomp-game exited with code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
