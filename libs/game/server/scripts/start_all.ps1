param(
    [string]$BuildDir,
    [string]$RunDir
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

$BinDir = Join-Path $BuildDir "bin"
$ConfigDir = Join-Path $BuildDir "libs/game/server/config"
$LogDir = Join-Path $RunDir "logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

function Start-GameService {
    param(
        [string]$Name,
        [string]$Exe,
        [string]$Config
    )

    $ExePath = Join-Path $BinDir $Exe
    if (-not (Test-Path $ExePath)) {
        $ExePath = Join-Path $BinDir ($Exe + ".exe")
    }
    if (-not (Test-Path $ExePath)) {
        throw "missing executable: $ExePath"
    }

    $ConfigPath = Join-Path $ConfigDir $Config
    if (-not (Test-Path $ConfigPath)) {
        throw "missing config: $ConfigPath"
    }

    $PidPath = Join-Path $RunDir ($Name + ".pid")
    if (Test-Path $PidPath) {
        $ExistingPid = Get-Content $PidPath -ErrorAction SilentlyContinue
        if ($ExistingPid) {
            $Existing = Get-Process -Id ([int]$ExistingPid) -ErrorAction SilentlyContinue
            if ($Existing) {
                Write-Host "$Name already running pid=$ExistingPid"
                return
            }
        }
    }

    $LogPath = Join-Path $LogDir ($Name + ".log")
    $Process = Start-Process -FilePath $ExePath -ArgumentList @($ConfigPath) -RedirectStandardOutput $LogPath -RedirectStandardError $LogPath -PassThru -WindowStyle Hidden
    Set-Content -Path $PidPath -Value $Process.Id
    Write-Host "started $Name pid=$($Process.Id) log=$LogPath"
}

# Startup order matters: tunnel first, db proxies second, business services last.
Start-GameService tunnel game_tunnel_server tunnel.json
Start-Sleep -Milliseconds 200
Start-GameService player_db_proxy game_player_db_proxy_server player_db_proxy.json
Start-GameService world_db_proxy game_world_db_proxy_server world_db_proxy.json
Start-GameService global_db_proxy game_global_db_proxy_server global_db_proxy.json
Start-Sleep -Milliseconds 200
Start-GameService global game_global_server global.json
Start-GameService world game_world_server world.json
Start-Sleep -Milliseconds 200
Start-GameService zone game_zone_server zone.json
Start-Sleep -Milliseconds 200
Start-GameService gateway game_gateway_server gateway.json
Start-GameService web game_web_server web.json
Start-GameService rank game_rank_server rank.json
Start-GameService chat_web game_chat_web_server chat_web.json

Write-Host "all game services started; pid files in $RunDir"
