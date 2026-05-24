param(
    [string]$ServerBin = $(if ($env:SERVER_BIN) { $env:SERVER_BIN } else { Join-Path $PSScriptRoot "bt_downloader.exe" }),
    [int]$AdminPort = $(if ($env:BT_ADMIN_PORT) { [int]$env:BT_ADMIN_PORT } else { 18080 })
)

$ErrorActionPreference = "Stop"

Write-Host "[1/3] Checking binary"
if (-not (Test-Path -LiteralPath $ServerBin)) {
    throw "server binary not found: $ServerBin"
}

Write-Host "[2/3] Checking server process"
$process = Get-Process bt_downloader -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $process) {
    throw "bt_downloader is not running"
}

Write-Host "[3/3] Checking admin API"
$url = "http://127.0.0.1:$AdminPort/admin/api/overview"
$response = Invoke-RestMethod -Uri $url -TimeoutSec 5
if (-not $response) {
    throw "admin API returned empty response"
}

Write-Host "health check passed"
