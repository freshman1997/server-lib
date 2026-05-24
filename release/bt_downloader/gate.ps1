param(
    [string]$ServerBin = $(if ($env:SERVER_BIN) { $env:SERVER_BIN } else { Join-Path $PSScriptRoot "bt_downloader.exe" }),
    [string]$ConfigFile = $(if ($env:CONFIG_FILE) { $env:CONFIG_FILE } else { Join-Path $PSScriptRoot "config.json" }),
    [string]$PidFile = $(if ($env:PID_FILE) { $env:PID_FILE } else { Join-Path $PSScriptRoot "bt_downloader.gate.pid" }),
    [string]$LogFile = $(if ($env:LOG_FILE) { $env:LOG_FILE } else { Join-Path $PSScriptRoot "bt_downloader.gate.log" }),
    [string]$ErrFile = $(if ($env:ERR_FILE) { $env:ERR_FILE } else { Join-Path $PSScriptRoot "bt_downloader.gate.err.log" }),
    [int]$AdminPort = $(if ($env:BT_ADMIN_PORT) { [int]$env:BT_ADMIN_PORT } else { 18080 })
)

$ErrorActionPreference = "Stop"

function Stop-GateProcess {
    if (Test-Path -LiteralPath $PidFile) {
        $pidText = (Get-Content -LiteralPath $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($pidText) {
            Stop-Process -Id ([int]$pidText) -Force -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
    }
}

try {
    Write-Host "[1/4] Start bt_downloader"
    if (-not (Test-Path -LiteralPath $ServerBin)) {
        throw "server binary not found: $ServerBin"
    }
    $env:YUAN_BT_ADMIN_PORT = [string]$AdminPort
    $env:YUAN_BT_ENABLE_SSL = "0"
    $process = Start-Process -FilePath $ServerBin `
        -ArgumentList @($ConfigFile) `
        -WorkingDirectory $PSScriptRoot `
        -RedirectStandardOutput $LogFile `
        -RedirectStandardError $ErrFile `
        -WindowStyle Hidden `
        -PassThru
    $process.Id | Set-Content -LiteralPath $PidFile -Encoding ASCII

    Write-Host "[2/4] Verify process alive"
    Start-Sleep -Milliseconds 800
    if (-not (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) {
        throw "bt_downloader exited early; see logs: $LogFile, $ErrFile"
    }

    Write-Host "[3/4] Verify admin API"
    $url = "http://127.0.0.1:$AdminPort/admin/api/overview"
    $ok = $false
    for ($i = 0; $i -lt 20; $i++) {
        try {
            $response = Invoke-RestMethod -Uri $url -TimeoutSec 3
            if ($response) {
                $ok = $true
                break
            }
        } catch {
            Start-Sleep -Milliseconds 250
        }
    }
    if (-not $ok) {
        throw "admin API did not become ready: $url"
    }

    Write-Host "[4/4] Stop server"
    Stop-GateProcess
    Write-Host "bt_downloader gate passed"
} finally {
    Stop-GateProcess
}
