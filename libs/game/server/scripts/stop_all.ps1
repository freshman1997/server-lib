param(
    [string]$BuildDir,
    [string]$RunDir,
    [int]$StopGraceSeconds = 30,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = Resolve-Path (Join-Path $ScriptDir "../../../..")
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $Root "build"
}
if ([string]::IsNullOrWhiteSpace($RunDir)) {
    $RunDir = Join-Path $BuildDir "game_server_run"
}

function Stop-GameService {
    param([string]$Name)

    $PidPath = Join-Path $RunDir ($Name + ".pid")
    if (-not (Test-Path $PidPath)) {
        return
    }

    $PidText = Get-Content $PidPath -ErrorAction SilentlyContinue
    if ([string]::IsNullOrWhiteSpace($PidText)) {
        Remove-Item $PidPath -Force -ErrorAction SilentlyContinue
        return
    }

    $Process = Get-Process -Id ([int]$PidText) -ErrorAction SilentlyContinue
    if ($Process) {
        if ($Process.CloseMainWindow()) {
            $Exited = $Process.WaitForExit($StopGraceSeconds * 1000)
            if (-not $Exited) {
                Write-Error "still stopping $Name pid=$PidText after ${StopGraceSeconds}s; leaving process alive so graceful shutdown can finish"
                return
            }
        } elseif ($Force) {
            Stop-Process -Id ([int]$PidText) -Force
            $Process.WaitForExit($StopGraceSeconds * 1000) | Out-Null
        } else {
            Write-Error "$Name pid=$PidText has no main window for graceful close; leaving process alive. Re-run with -Force only if graceful shutdown is impossible."
            return
        }
        Write-Host "stopped $Name pid=$PidText"
    }

    Remove-Item $PidPath -Force -ErrorAction SilentlyContinue
}

# Shutdown order matters: stop external entries and business services first,
# keep db proxies available for final data flush, then stop tunnel last.
Stop-GameService chat_web
Stop-GameService web
Stop-GameService gateway
Stop-GameService rank
Stop-GameService zone
Stop-GameService world
Stop-GameService global
Stop-GameService global_db_proxy
Stop-GameService world_db_proxy
Stop-GameService player_db_proxy
Stop-GameService tunnel

Write-Host "all game services stopped"
