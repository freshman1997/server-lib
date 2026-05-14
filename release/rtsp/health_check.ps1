param(
    [string]$BuildDir = "",
    [int]$RtspPort = 554
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-BuildDir {
    param([string]$InputDir)

    if ($InputDir -and $InputDir.Trim().Length -gt 0) {
        return [System.IO.Path]::GetFullPath($InputDir)
    }
    if ($env:YUAN_BUILD_DIR -and $env:YUAN_BUILD_DIR.Trim().Length -gt 0) {
        return [System.IO.Path]::GetFullPath($env:YUAN_BUILD_DIR)
    }

    $repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
    $mingwDir = Join-Path $repoRoot "build-mingw"
    if (Test-Path -LiteralPath $mingwDir) {
        return $mingwDir
    }
    return (Join-Path $repoRoot "build")
}

$resolvedBuildDir = Resolve-BuildDir -InputDir $BuildDir
$serverBin = Join-Path $resolvedBuildDir "release\rtsp\release_rtsp_server.exe"

"[1/4] Checking binary"
if (-not (Test-Path -LiteralPath $serverBin)) {
    throw "binary not found: $serverBin"
}

"[2/4] Checking server process"
$proc = Get-Process -Name "release_rtsp_server" -ErrorAction SilentlyContinue
if (-not $proc) {
    throw "release_rtsp_server is not running"
}

"[3/4] Checking listen port ($RtspPort)"
$conn = Get-NetTCPConnection -LocalPort $RtspPort -State Listen -ErrorAction SilentlyContinue
if (-not $conn) {
    throw "RTSP port not listening: $RtspPort"
}

"[4/4] Checking RTSP/RTCP gate"
& pwsh -File (Join-Path $PSScriptRoot "scripts\run_rtsp_gate.ps1") -BuildDir $resolvedBuildDir -Regex "rtsp|rtcp" | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "gate failed"
}

"health check passed"
