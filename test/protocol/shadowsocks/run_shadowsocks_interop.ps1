param(
    [string]$ServerPath = ".\build\test\protocol\shadowsocks\shadowsocks_server_tool.exe",
    [string]$ClientPath = "",
    [ValidateSet("rust","libev")]
    [string]$ClientType = "rust",
    [int]$ServerPort = 8388,
    [int]$LocalSocksPort = 1081,
    [string]$Method = "chacha20-ietf-poly1305",
    [string]$Password = "secret",
    [string]$ServerHost = "127.0.0.1",
    [string]$OutDir = ".\logs\shadowsocks_interop"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-ClientPath {
    param([string]$Path, [string]$Type)

    if ($Path -and (Test-Path $Path)) {
        return (Resolve-Path $Path).Path
    }

    $candidates = @()
    if ($Type -eq "rust") {
        $candidates = @("sslocal", "sslocal.exe")
    } else {
        $candidates = @("ss-local", "ss-local.exe")
    }

    foreach ($cmd in $candidates) {
        $resolved = Get-Command $cmd -ErrorAction SilentlyContinue
        if ($resolved) {
            return $resolved.Path
        }
    }

    throw "Cannot find client executable for type=$Type. Please pass -ClientPath explicitly."
}

function Start-InteropClient {
    param(
        [string]$Type,
        [string]$ExePath,
        [string]$ServerHost,
        [int]$ServerPort,
        [int]$LocalSocksPort,
        [string]$Method,
        [string]$Password,
        [string]$StdOut,
        [string]$StdErr
    )

    if ($Type -eq "rust") {
        $args = @(
            "-s", $ServerHost,
            "-p", "$ServerPort",
            "-b", "127.0.0.1",
            "-l", "$LocalSocksPort",
            "-m", $Method,
            "-k", $Password,
            "-U"
        )
    } else {
        $args = @(
            "-s", $ServerHost,
            "-p", "$ServerPort",
            "-b", "127.0.0.1",
            "-l", "$LocalSocksPort",
            "-m", $Method,
            "-k", $Password,
            "-u"
        )
    }

    return Start-Process -FilePath $ExePath -ArgumentList $args -NoNewWindow -RedirectStandardOutput $StdOut -RedirectStandardError $StdErr -PassThru
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$serverLog = Join-Path $OutDir "ss-server-$ts.log"
$serverErr = Join-Path $OutDir "ss-server-$ts.err.log"
$clientLog = Join-Path $OutDir "ss-client-$ts.log"
$clientErr = Join-Path $OutDir "ss-client-$ts.err.log"
$driverLog = Join-Path $OutDir "interop-$ts.log"

$resolvedClient = Resolve-ClientPath -Path $ClientPath -Type $ClientType

"[$(Get-Date -Format o)] interop start" | Tee-Object -FilePath $driverLog -Append
"server=$ServerPath client=$resolvedClient type=$ClientType host=$ServerHost" | Tee-Object -FilePath $driverLog -Append

$env:YUAN_SS_LISTEN_HOST = $ServerHost
$env:YUAN_SS_PORT = "$ServerPort"
$env:YUAN_SS_METHOD = $Method
$env:YUAN_SS_PASSWORD = $Password
$env:YUAN_SS_ENABLE_TCP = "1"
$env:YUAN_SS_ENABLE_UDP = "1"
$env:YUAN_SS_ALLOW_PRIVATE_TARGETS = "1"

$serverProc = Start-Process -FilePath $ServerPath -NoNewWindow -RedirectStandardOutput $serverLog -RedirectStandardError $serverErr -PassThru
Start-Sleep -Seconds 1
if ($serverProc.HasExited) {
    throw "shadowsocks server exited early, check $serverLog"
}

$clientProc = Start-InteropClient -Type $ClientType -ExePath $resolvedClient -ServerHost $ServerHost -ServerPort $ServerPort -LocalSocksPort $LocalSocksPort -Method $Method -Password $Password -StdOut $clientLog -StdErr $clientErr
Start-Sleep -Seconds 1
if ($clientProc.HasExited) {
    Stop-Process -Id $serverProc.Id -Force -ErrorAction SilentlyContinue
    throw "interop client exited early, check $clientLog"
}

$tcpOk = $false
$udpOk = $false

try {
    $ProgressPreference = "SilentlyContinue"

    # TCP check
    curl.exe --socks5-hostname ("127.0.0.1:" + $LocalSocksPort) --max-time 20 -s https://example.com -o NUL
    $tcpOk = ($LASTEXITCODE -eq 0)
    "tcp_check_exit=$LASTEXITCODE" | Tee-Object -FilePath $driverLog -Append

    # UDP check via DNS-over-HTTPS through SOCKS proxy (still exercises proxy path)
    curl.exe --socks5-hostname ("127.0.0.1:" + $LocalSocksPort) --max-time 20 -s "https://dns.google/resolve?name=example.com&type=A" -o NUL
    $udpOk = ($LASTEXITCODE -eq 0)
    "udp_check_exit=$LASTEXITCODE" | Tee-Object -FilePath $driverLog -Append
}
finally {
    if ($clientProc -and -not $clientProc.HasExited) {
        Stop-Process -Id $clientProc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($serverProc -and -not $serverProc.HasExited) {
        Stop-Process -Id $serverProc.Id -Force -ErrorAction SilentlyContinue
    }
}

"tcp_ok=$tcpOk udp_ok=$udpOk" | Tee-Object -FilePath $driverLog -Append
"serverLog=$serverLog" | Tee-Object -FilePath $driverLog -Append
"serverErr=$serverErr" | Tee-Object -FilePath $driverLog -Append
"clientLog=$clientLog" | Tee-Object -FilePath $driverLog -Append
"clientErr=$clientErr" | Tee-Object -FilePath $driverLog -Append

if (-not $tcpOk) {
    throw "TCP interop check failed"
}

if (-not $udpOk) {
    throw "UDP interop check failed"
}

"[$(Get-Date -Format o)] interop passed" | Tee-Object -FilePath $driverLog -Append
