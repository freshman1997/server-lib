param(
    [string]$PidFile = $env:PID_FILE
)

$ErrorActionPreference = "Stop"

if (-not $PidFile) {
    $PidFile = Join-Path $PSScriptRoot "release_nas_server.pid"
}

if (-not (Test-Path $PidFile)) {
    Write-Host "release_nas_server pid file not found: $PidFile"
    exit 0
}

$pidText = (Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
if (-not $pidText) {
    Remove-Item -LiteralPath $PidFile -Force
    exit 0
}

$process = Get-Process -Id ([int]$pidText) -ErrorAction SilentlyContinue
if ($process) {
    Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    try {
        Wait-Process -Id $process.Id -Timeout 5 -ErrorAction SilentlyContinue
    } catch {
    }
    $process = Get-Process -Id ([int]$pidText) -ErrorAction SilentlyContinue
    if ($process) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}

Remove-Item -LiteralPath $PidFile -Force
Write-Host "release_nas_server stopped: $pidText"
