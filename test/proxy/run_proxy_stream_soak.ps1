param(
    [string]$ProxyServerPath = ".\build\test\proxy\proxy_server.exe",
    [int]$ProxyPort = 3128,
    [int]$UpstreamPort = 18080,
    [int]$DurationSec = 1800,
    [int]$Concurrency = 6,
    [int]$ClientStreamSec = 30,
    [int]$CooldownSec = 60,
    [string]$OutDir = ".\logs\proxy_stream_soak"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$proxyLog = Join-Path $OutDir "proxy-$ts.log"
$proxyErrLog = Join-Path $OutDir "proxy-$ts.err.log"
$driverLog = Join-Path $OutDir "driver-$ts.log"
$stopFlag = Join-Path $OutDir "stream-stop-$ts.flag"

"[$(Get-Date -Format o)] starting stream soak" | Tee-Object -FilePath $driverLog -Append
"proxy=$ProxyServerPath proxy_port=$ProxyPort upstream_port=$UpstreamPort duration=$DurationSec concurrency=$Concurrency cooldown=$CooldownSec" | Tee-Object -FilePath $driverLog -Append

$streamJob = Start-Job -ScriptBlock {
    param($Port, $StopFile)

    Add-Type -AssemblyName System.Net.Sockets

    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
    $listener.Server.SetSocketOption([System.Net.Sockets.SocketOptionLevel]::Socket, [System.Net.Sockets.SocketOptionName]::ReuseAddress, $true)
    $listener.Start()

    try {
        while (-not (Test-Path $StopFile)) {
            if (-not $listener.Pending()) {
                Start-Sleep -Milliseconds 100
                continue
            }

            $client = $listener.AcceptTcpClient()
            try {
                $client.NoDelay = $true
                $stream = $client.GetStream()

                $buffer = New-Object byte[] 4096
                $request = New-Object System.Text.StringBuilder
                while ($client.Connected -and -not (Test-Path $StopFile)) {
                    if (-not $stream.DataAvailable) {
                        Start-Sleep -Milliseconds 10
                        continue
                    }
                    $read = $stream.Read($buffer, 0, $buffer.Length)
                    if ($read -le 0) {
                        break
                    }
                    [void]$request.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $read))
                    if ($request.ToString().Contains("`r`n`r`n")) {
                        break
                    }
                }

                $headers = "HTTP/1.1 200 OK`r`nContent-Type: video/mp2t`r`nTransfer-Encoding: chunked`r`nConnection: close`r`n`r`n"
                $h = [System.Text.Encoding]::ASCII.GetBytes($headers)
                $stream.Write($h, 0, $h.Length)

                $rng = [System.Random]::new()
                $payload = New-Object byte[] 65536

                while ($client.Connected -and -not (Test-Path $StopFile)) {
                    $rng.NextBytes($payload)
                    $prefix = [System.Text.Encoding]::ASCII.GetBytes(("{0:X}`r`n" -f $payload.Length))
                    $suffix = [System.Text.Encoding]::ASCII.GetBytes("`r`n")

                    $stream.Write($prefix, 0, $prefix.Length)
                    $stream.Write($payload, 0, $payload.Length)
                    $stream.Write($suffix, 0, $suffix.Length)
                    $stream.Flush()

                    Start-Sleep -Milliseconds 200
                }
            }
            catch {
            }
            finally {
                if ($client) {
                    $client.Close()
                }
            }
        }
    }
    finally {
        $listener.Stop()
    }
} -ArgumentList $UpstreamPort, $stopFlag

$env:YUAN_PROXY_LISTEN_HOST = "127.0.0.1"
$env:YUAN_PROXY_LISTEN_PORT = "$ProxyPort"
$env:YUAN_PROXY_ALLOW_PRIVATE_TARGETS = "true"
$env:YUAN_PROXY_IDLE_TIMEOUT_MS = "120000"
$env:YUAN_PROXY_HEADER_TIMEOUT_MS = "15000"
$env:YUAN_PROXY_CONNECT_TIMEOUT_MS = "10000"
$env:YUAN_PROXY_DRAIN_TIMEOUT_MS = "5000"

$proxyProc = Start-Process -FilePath $ProxyServerPath -NoNewWindow -RedirectStandardOutput $proxyLog -RedirectStandardError $proxyErrLog -PassThru

Start-Sleep -Seconds 2
if ($proxyProc.HasExited) {
    New-Item -Path $stopFlag -ItemType File -Force | Out-Null
    Stop-Job -Job $streamJob -ErrorAction SilentlyContinue | Out-Null
    Receive-Job -Job $streamJob -ErrorAction SilentlyContinue | Out-Null
    Remove-Job -Job $streamJob -Force | Out-Null
    throw "proxy server exited early, see $proxyLog"
}

$deadline = (Get-Date).AddSeconds($DurationSec)
$target = "http://127.0.0.1:$UpstreamPort/video"

try {
    while ((Get-Date) -lt $deadline) {
        $jobs = @()
        for ($i = 0; $i -lt $Concurrency; $i++) {
            $jobs += Start-Job -ScriptBlock {
                param($p, $u, $sec)
                $ProgressPreference = "SilentlyContinue"
                curl.exe -x ("http://127.0.0.1:" + $p) --http1.1 --max-time $sec -s -o NUL $u
                return $LASTEXITCODE
            } -ArgumentList $ProxyPort, $target, $ClientStreamSec
        }

        Wait-Job -Job $jobs | Out-Null
        $results = @(Receive-Job -Job $jobs)
        Remove-Job -Job $jobs

        $ok = @($results | Where-Object { $_ -eq 0 -or $_ -eq 28 }).Count
        $fail = @($results).Count - $ok
        "[$(Get-Date -Format o)] stream batch ok=$ok fail=$fail" | Tee-Object -FilePath $driverLog -Append
    }

    if ($CooldownSec -gt 0) {
        "[$(Get-Date -Format o)] cooldown start sec=$CooldownSec" | Tee-Object -FilePath $driverLog -Append
        Start-Sleep -Seconds $CooldownSec
        "[$(Get-Date -Format o)] cooldown done" | Tee-Object -FilePath $driverLog -Append
    }
}
finally {
    New-Item -Path $stopFlag -ItemType File -Force | Out-Null

    if (-not $proxyProc.HasExited) {
        Stop-Process -Id $proxyProc.Id -Force
    }

    Stop-Job -Job $streamJob -ErrorAction SilentlyContinue | Out-Null
    Receive-Job -Job $streamJob -ErrorAction SilentlyContinue | Out-Null
    Remove-Job -Job $streamJob -Force | Out-Null
}

"[$(Get-Date -Format o)] stream soak finished" | Tee-Object -FilePath $driverLog -Append
"proxyLog=$proxyLog" | Tee-Object -FilePath $driverLog -Append
"proxyErrLog=$proxyErrLog" | Tee-Object -FilePath $driverLog -Append
