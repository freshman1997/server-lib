param(
    [string]$ConfigPath = $env:YUAN_NAS_CONFIG,
    [string]$ServerBin = $env:SERVER_BIN,
    [string]$PidFile = $env:PID_FILE,
    [string]$LogFile = $env:LOG_FILE,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ExtraArgs
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

function Quote-Arg {
    param([string]$Value)
    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

if (-not $ConfigPath) {
    $ConfigPath = Join-Path $PSScriptRoot "config.json"
}
if (-not $PidFile) {
    $PidFile = Join-Path $PSScriptRoot "release_nas_server.pid"
}
if (-not $LogFile) {
    $LogFile = Join-Path $PSScriptRoot "release_nas_server.log"
}

$ServerBin = Resolve-NasBinary $ServerBin
if (-not (Test-Path $ServerBin)) {
    throw "server binary not found: $ServerBin"
}
if (-not (Test-Path $ConfigPath)) {
    throw "config file not found: $ConfigPath"
}

if (Test-Path $PidFile) {
    $existingPid = (Get-Content $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($existingPid) {
        $existing = Get-Process -Id ([int]$existingPid) -ErrorAction SilentlyContinue
        if ($existing) {
            Write-Host "release_nas_server is already running: $existingPid"
            exit 0
        }
    }
    Remove-Item -LiteralPath $PidFile -Force
}

$logDir = Split-Path -Parent $LogFile
if ($logDir) {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
}
$errFile = "$LogFile.err"
$arguments = @("--config", $ConfigPath) + $ExtraArgs
$argumentLine = ($arguments | ForEach-Object { Quote-Arg $_ }) -join " "

$process = Start-Process -FilePath $ServerBin `
    -ArgumentList $argumentLine `
    -RedirectStandardOutput $LogFile `
    -RedirectStandardError $errFile `
    -WindowStyle Hidden `
    -PassThru

Set-Content -LiteralPath $PidFile -Value $process.Id
Write-Host "release_nas_server started: $($process.Id)"
Write-Host "log: $LogFile"
