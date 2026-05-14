param(
    [string]$BuildDir = "",
    [int]$WebrtcPort = 9000
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
$serverBin = Join-Path $resolvedBuildDir "release\webrtc\release_webrtc_server.exe"

"[1/4] Checking binary"
if (-not (Test-Path -LiteralPath $serverBin)) {
    throw "binary not found: $serverBin"
}

"[2/4] Checking server process"
$proc = Get-Process -Name "release_webrtc_server" -ErrorAction SilentlyContinue
if (-not $proc) {
    throw "release_webrtc_server is not running"
}

"[3/4] Checking listen port ($WebrtcPort)"
$conn = Get-NetTCPConnection -LocalPort $WebrtcPort -State Listen -ErrorAction SilentlyContinue
if (-not $conn) {
    throw "WebRTC port not listening: $WebrtcPort"
}

"[4/4] Checking self-check endpoint"
& $serverBin --self-check-only --probe-host 127.0.0.1 --port $WebrtcPort | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "self-check failed"
}

"health check passed"
