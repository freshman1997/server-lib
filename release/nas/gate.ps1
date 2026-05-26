param(
    [int]$NasPort = $(if ($env:NAS_PORT) { [int]$env:NAS_PORT } else { 8080 }),
    [string]$RedisHost = $(if ($env:REDIS_HOST) { $env:REDIS_HOST } else { "127.0.0.1" }),
    [int]$RedisPort = $(if ($env:REDIS_PORT) { [int]$env:REDIS_PORT } else { 6379 }),
    [string]$AdminUser = $env:YUAN_NAS_ADMIN_USER,
    [string]$AdminPassword = $env:YUAN_NAS_ADMIN_PASSWORD,
    [string]$ConfigPath = $env:YUAN_NAS_CONFIG,
    [string]$ServerBin = $env:SERVER_BIN,
    [string]$PidFile = $env:PID_FILE,
    [string]$LogFile = $env:LOG_FILE
)

$ErrorActionPreference = "Stop"

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

function Get-BasicAuthHeader {
    param([string]$User, [string]$Password)
    if (-not $User -or -not $Password) {
        return $null
    }
    $token = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("${User}:${Password}"))
    return @{ Authorization = "Basic $token" }
}

if (-not $ConfigPath) {
    $ConfigPath = Join-Path $PSScriptRoot "config.json"
}
if (-not $PidFile) {
    $PidFile = Join-Path $PSScriptRoot "release_nas_server.gate.pid"
}
if (-not $LogFile) {
    $LogFile = Join-Path $PSScriptRoot "release_nas_server.gate.log"
}

try {
    Write-Host "[1/6] verify Redis (${RedisHost}:$RedisPort)"
    if (-not (Test-TcpPort $RedisHost $RedisPort)) {
        Write-Host "SKIP: Redis is not reachable; production NAS gate requires metadata storage"
        exit 77
    }

    Write-Host "[2/6] start release_nas_server"
    & (Join-Path $PSScriptRoot "start.ps1") -ConfigPath $ConfigPath -ServerBin $ServerBin -PidFile $PidFile -LogFile $LogFile
    Start-Sleep -Milliseconds 700

    Write-Host "[3/6] verify process alive"
    $pidText = (Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if (-not $pidText -or -not (Get-Process -Id ([int]$pidText) -ErrorAction SilentlyContinue)) {
        throw "release_nas_server is not running after start"
    }

    Write-Host "[4/6] verify listen port ($NasPort)"
    if (-not (Test-TcpPort "127.0.0.1" $NasPort)) {
        throw "TCP port is not reachable: $NasPort"
    }

    Write-Host "[5/6] verify health endpoint"
    $response = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$NasPort/nas/health" -TimeoutSec 5
    if ($response.StatusCode -ne 200 -or $response.Content -notmatch '"ok"\s*:\s*true') {
        throw "health endpoint did not report ok=true"
    }

    Write-Host "[6/6] verify admin readiness"
    $headers = Get-BasicAuthHeader -User $AdminUser -Password $AdminPassword
    if ($headers) {
        $readiness = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$NasPort/nas/admin/readiness" -Headers $headers -TimeoutSec 5
        if ($readiness.StatusCode -ne 200 -or $readiness.Content -notmatch '"ready"\s*:\s*true') {
            throw "admin readiness did not report ready=true"
        }
    } elseif ($env:YUAN_NAS_GATE_REQUIRE_READINESS -eq "1") {
        throw "YUAN_NAS_ADMIN_USER/YUAN_NAS_ADMIN_PASSWORD are required when YUAN_NAS_GATE_REQUIRE_READINESS=1"
    } else {
        Write-Host "SKIP: set YUAN_NAS_ADMIN_USER and YUAN_NAS_ADMIN_PASSWORD to verify /nas/admin/readiness"
    }

    Write-Host "nas gate passed"
} finally {
    & (Join-Path $PSScriptRoot "stop.ps1") -PidFile $PidFile
}
