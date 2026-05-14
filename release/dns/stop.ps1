param(
    [string]$PidFile = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$resolvedPid = $PidFile
if (-not $resolvedPid -or $resolvedPid.Trim().Length -eq 0) {
    $resolvedPid = Join-Path $PSScriptRoot "release_dns_server.pid"
}
$resolvedPid = [System.IO.Path]::GetFullPath($resolvedPid)

if (-not (Test-Path -LiteralPath $resolvedPid)) {
    "release_dns_server pid file not found: $resolvedPid"
    exit 0
}

$pidText = (Get-Content -LiteralPath $resolvedPid -Raw).Trim()
if (-not $pidText) {
    Remove-Item -LiteralPath $resolvedPid -Force -ErrorAction SilentlyContinue
    exit 0
}

$pid = [int]$pidText
try {
    $proc = Get-Process -Id $pid -ErrorAction Stop
    if ($null -ne $proc -and -not $proc.HasExited) {
        Stop-Process -Id $pid -Force
    }
} catch {
}

Remove-Item -LiteralPath $resolvedPid -Force -ErrorAction SilentlyContinue
"release_dns_server stopped: $pid"
