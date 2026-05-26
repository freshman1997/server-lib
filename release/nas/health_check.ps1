param(
    [int]$NasPort = $(if ($env:NAS_PORT) { [int]$env:NAS_PORT } else { 8080 }),
    [string]$ServerBin = $env:SERVER_BIN,
    [string]$PidFile = $env:PID_FILE
)

$ErrorActionPreference = "Stop"

function Resolve-NasBinary {
    param([string]$Requested)
    if ($Requested) {
        return $Requested
    }
    $candidates = @(
        (Join-Path $PSScriptRoot "release_nas_server.exe"),
        (Join-Path $PSScriptRoot "release_nas_server"),
        (Join-Path $PSScriptRoot "Release\release_nas_server.exe"),
        (Join-Path $PSScriptRoot "RelWithDebInfo\release_nas_server.exe"),
        (Join-Path $PSScriptRoot "MinSizeRel\release_nas_server.exe"),
        (Join-Path $PSScriptRoot "Debug\release_nas_server.exe"),
        (Join-Path $PSScriptRoot "..\..\build\release\nas\Release\release_nas_server.exe"),
        (Join-Path $PSScriptRoot "..\..\build\release\nas\release_nas_server.exe"),
        (Join-Path $PSScriptRoot "..\..\build-windows-vs\release\nas\Release\release_nas_server.exe"),
        (Join-Path $PSScriptRoot "..\..\build-windows-mingw\release\nas\release_nas_server.exe"),
        (Join-Path $PSScriptRoot "..\..\build-mingw-nas-release\release\nas\release_nas_server.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $candidates[0]
}

function Test-TcpPort {
    param([string]$HostName, [int]$Port)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $iar = $client.BeginConnect($HostName, $Port, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(2000, $false)) {
            return $false
        }
        $client.EndConnect($iar)
        return $true
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

if (-not $PidFile) {
    $PidFile = Join-Path $PSScriptRoot "release_nas_server.pid"
}
$ServerBin = Resolve-NasBinary $ServerBin

Write-Host "[1/4] Checking binary"
if (-not (Test-Path $ServerBin)) {
    throw "server binary not found: $ServerBin"
}

Write-Host "[2/4] Checking server process"
if (Test-Path $PidFile) {
    $pidText = (Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if (-not $pidText -or -not (Get-Process -Id ([int]$pidText) -ErrorAction SilentlyContinue)) {
        throw "release_nas_server pid is not running"
    }
} elseif (-not (Get-Process -Name "release_nas_server" -ErrorAction SilentlyContinue)) {
    throw "release_nas_server is not running"
}

Write-Host "[3/4] Checking TCP listen port ($NasPort)"
if (-not (Test-TcpPort "127.0.0.1" $NasPort)) {
    throw "TCP port is not reachable: $NasPort"
}

Write-Host "[4/4] Checking health endpoint"
$response = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$NasPort/nas/health" -TimeoutSec 5
if ($response.StatusCode -ne 200 -or $response.Content -notmatch '"ok"\s*:\s*true') {
    throw "health endpoint did not report ok=true"
}

Write-Host "health check passed"
