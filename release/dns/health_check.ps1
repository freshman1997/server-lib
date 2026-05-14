param(
    [string]$BuildDir = "",
    [int]$DnsPort = 5353
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

"[1/3] Checking binary"
if (-not (Test-Path -LiteralPath $serverBin)) {
    throw "binary not found: $serverBin"
}

"[2/3] Checking server process"
$proc = Get-Process -Name "release_dns_server" -ErrorAction SilentlyContinue
if (-not $proc) {
    throw "release_dns_server is not running"
}

"[3/3] Checking UDP listen port ($DnsPort)"
$udpRows = netstat -ano -p udp | Out-String
if ($udpRows -notmatch (":$DnsPort\s")) {
    throw "DNS UDP port not listening: $DnsPort"
}

"health check passed"
