param(
    [string]$ProxyServerPath = ".\build\test\proxy\proxy_server.exe",
    [int]$ProxyPort = 3128,
    [int]$DurationSec = 1800,
    [int]$Concurrency = 8,
    [int]$RequestTimeoutSec = 20,
    [string]$OutDir = ".\logs\proxy_soak"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$proxyLog = Join-Path $OutDir "proxy-$ts.log"
$proxyErrLog = Join-Path $OutDir "proxy-$ts.err.log"
$driverLog = Join-Path $OutDir "driver-$ts.log"

"[$(Get-Date -Format o)] starting soak test" | Tee-Object -FilePath $driverLog -Append
"proxy=$ProxyServerPath port=$ProxyPort duration=$DurationSec concurrency=$Concurrency" | Tee-Object -FilePath $driverLog -Append

$env:YUAN_PROXY_LISTEN_HOST = "127.0.0.1"
$env:YUAN_PROXY_LISTEN_PORT = "$ProxyPort"
$env:YUAN_PROXY_ALLOW_PRIVATE_TARGETS = "true"
$env:YUAN_PROXY_IDLE_TIMEOUT_MS = "120000"
$env:YUAN_PROXY_HEADER_TIMEOUT_MS = "15000"
$env:YUAN_PROXY_CONNECT_TIMEOUT_MS = "10000"
$env:YUAN_PROXY_DRAIN_TIMEOUT_MS = "5000"

$proc = Start-Process -FilePath $ProxyServerPath -NoNewWindow -RedirectStandardOutput $proxyLog -RedirectStandardError $proxyErrLog -PassThru

Start-Sleep -Seconds 2
if ($proc.HasExited) {
    throw "proxy server exited early, see $proxyLog"
}

$deadline = (Get-Date).AddSeconds($DurationSec)
$targets = @(
    "https://example.com/",
    "https://www.cloudflare.com/",
    "https://www.wikipedia.org/"
)

try {
    while ((Get-Date) -lt $deadline) {
        $jobs = @()
        for ($i = 0; $i -lt $Concurrency; $i++) {
            $url = $targets[$i % $targets.Count]
            $jobs += Start-Job -ScriptBlock {
                param($u, $p, $to)
                $ProgressPreference = "SilentlyContinue"
                curl.exe -x ("http://127.0.0.1:" + $p) --max-time $to -s -o NUL $u
                return $LASTEXITCODE
            } -ArgumentList $url, $ProxyPort, $RequestTimeoutSec
        }

        Wait-Job -Job $jobs | Out-Null
        $results = @(Receive-Job -Job $jobs)
        Remove-Job -Job $jobs -Force

        $ok = @($results | Where-Object { $_ -eq 0 }).Count
        $fail = @($results).Count - $ok
        "[$(Get-Date -Format o)] batch ok=$ok fail=$fail" | Tee-Object -FilePath $driverLog -Append

        Start-Sleep -Milliseconds 500
    }
}
finally {
    if (-not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
    }
}

"[$(Get-Date -Format o)] soak finished" | Tee-Object -FilePath $driverLog -Append
"proxyLog=$proxyLog" | Tee-Object -FilePath $driverLog -Append
"proxyErrLog=$proxyErrLog" | Tee-Object -FilePath $driverLog -Append
