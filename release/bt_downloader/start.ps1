param(
    [string]$ServerBin = $(if ($env:SERVER_BIN) { $env:SERVER_BIN } else { Join-Path $PSScriptRoot "bt_downloader.exe" }),
    [string]$Config = $(if ($env:YUAN_BT_CONFIG) { $env:YUAN_BT_CONFIG } else { Join-Path $PSScriptRoot "config.json" }),
    [string]$PidFile = $(if ($env:PID_FILE) { $env:PID_FILE } else { Join-Path $PSScriptRoot "bt_downloader.pid" }),
    [string]$LogFile = $(if ($env:LOG_FILE) { $env:LOG_FILE } else { Join-Path $PSScriptRoot "bt_downloader.log" }),
    [string]$ErrFile = $(if ($env:ERR_FILE) { $env:ERR_FILE } else { Join-Path $PSScriptRoot "bt_downloader.err.log" })
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ServerBin)) {
    throw "server binary not found: $ServerBin"
}

if (Test-Path -LiteralPath $PidFile) {
    $oldPid = (Get-Content -LiteralPath $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($oldPid) {
        $oldProcess = Get-Process -Id ([int]$oldPid) -ErrorAction SilentlyContinue
        if ($oldProcess) {
            Write-Host "bt_downloader is already running: $oldPid"
            exit 0
        }
    }
    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
}

$logDir = Split-Path -Parent $LogFile
if ($logDir -and -not (Test-Path -LiteralPath $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

$process = Start-Process -FilePath $ServerBin `
    -ArgumentList @($Config) `
    -WorkingDirectory $PSScriptRoot `
    -RedirectStandardOutput $LogFile `
    -RedirectStandardError $ErrFile `
    -WindowStyle Hidden `
    -PassThru

$process.Id | Set-Content -LiteralPath $PidFile -Encoding ASCII
Write-Host "bt_downloader started: $($process.Id)"
Write-Host "log: $LogFile"
Write-Host "err: $ErrFile"
