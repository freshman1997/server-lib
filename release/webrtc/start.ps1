param(
    [string]$BuildDir = "",
    [string]$Config = "",
    [string]$LogFile = "",
    [string]$PidFile = ""
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
$bin = Join-Path $resolvedBuildDir "release\webrtc\release_webrtc_server.exe"
if (-not (Test-Path -LiteralPath $bin)) {
    throw "release_webrtc_server.exe not found: $bin"
}

$resolvedConfig = $Config
if (-not $resolvedConfig -or $resolvedConfig.Trim().Length -eq 0) {
    if ($env:YUAN_WEBRTC_CONFIG -and $env:YUAN_WEBRTC_CONFIG.Trim().Length -gt 0) {
        $resolvedConfig = $env:YUAN_WEBRTC_CONFIG
    } else {
        $resolvedConfig = Join-Path $PSScriptRoot "config.json"
    }
}
$resolvedConfig = [System.IO.Path]::GetFullPath($resolvedConfig)
if (-not (Test-Path -LiteralPath $resolvedConfig)) {
    throw "config not found: $resolvedConfig"
}

$resolvedLog = $LogFile
if (-not $resolvedLog -or $resolvedLog.Trim().Length -eq 0) {
    $resolvedLog = Join-Path $PSScriptRoot "release_webrtc_server.log"
}
$resolvedLog = [System.IO.Path]::GetFullPath($resolvedLog)
$resolvedErrLog = "$resolvedLog.err"

$resolvedPid = $PidFile
if (-not $resolvedPid -or $resolvedPid.Trim().Length -eq 0) {
    $resolvedPid = Join-Path $PSScriptRoot "release_webrtc_server.pid"
}
$resolvedPid = [System.IO.Path]::GetFullPath($resolvedPid)

if (Test-Path -LiteralPath $resolvedPid) {
    $existing = (Get-Content -LiteralPath $resolvedPid -Raw).Trim()
    if ($existing) {
        try {
            $proc = Get-Process -Id ([int]$existing) -ErrorAction Stop
            if ($null -ne $proc -and -not $proc.HasExited) {
                "release_webrtc_server is already running: $existing"
                exit 0
            }
        } catch {
        }
    }
    Remove-Item -LiteralPath $resolvedPid -Force -ErrorAction SilentlyContinue
}

$logDir = Split-Path -Parent $resolvedLog
if ($logDir -and -not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}

$proc = Start-Process -FilePath $bin -ArgumentList @("--config", $resolvedConfig) -RedirectStandardOutput $resolvedLog -RedirectStandardError $resolvedErrLog -PassThru -WindowStyle Hidden
Set-Content -LiteralPath $resolvedPid -Value $proc.Id -Encoding ascii

"release_webrtc_server started: $($proc.Id)"
"log: $resolvedLog"
"err: $resolvedErrLog"
