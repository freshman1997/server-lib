param(
    [string]$BuildDir = "",
    [string]$ConfigFile = "",
    [int]$DnsPort = 5353,
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
$serverBin = Join-Path $resolvedBuildDir "release\dns\release_dns_server.exe"
if (-not (Test-Path -LiteralPath $serverBin)) {
    throw "binary not found: $serverBin"
}

if (-not $ConfigFile -or $ConfigFile.Trim().Length -eq 0) {
    $ConfigFile = Join-Path $PSScriptRoot "config.json"
}
$ConfigFile = [System.IO.Path]::GetFullPath($ConfigFile)

if (-not $LogFile -or $LogFile.Trim().Length -eq 0) {
    $LogFile = Join-Path $PSScriptRoot "release_dns_server.gate.log"
}
$LogFile = [System.IO.Path]::GetFullPath($LogFile)

if (-not $PidFile -or $PidFile.Trim().Length -eq 0) {
    $PidFile = Join-Path $PSScriptRoot "release_dns_server.gate.pid"
}
$PidFile = [System.IO.Path]::GetFullPath($PidFile)

$proc = $null
try {
    "[1/4] start release_dns_server"
    $proc = Start-Process -FilePath $serverBin -ArgumentList @("--config", $ConfigFile, "--port", "$DnsPort") -RedirectStandardOutput $LogFile -RedirectStandardError "$LogFile.err" -PassThru -WindowStyle Hidden
    Set-Content -LiteralPath $PidFile -Value $proc.Id -Encoding ascii
    Start-Sleep -Milliseconds 300

    "[2/4] run in-process self-check"
    & $serverBin --port $DnsPort --self-check-only | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "self-check command failed"
    }

    "[3/4] verify process still alive"
    $running = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
    if (-not $running -or $running.HasExited) {
        throw "server process exited unexpectedly"
    }

    "[4/4] stop server"
    Stop-Process -Id $proc.Id -Force
    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
    "dns gate passed"
}
finally {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
}
