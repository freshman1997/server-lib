param(
    [Parameter(Mandatory = $true)]
    [string]$ProxyLog,
    [int]$IdleTailSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not (Test-Path $ProxyLog)) {
    throw "log not found: $ProxyLog"
}

$pattern = 'traffic aggregate 1s active=(\d+) up_Bps=(\d+) down_Bps=(\d+) total_up=(\d+) total_down=(\d+) tunnel_mem=(\d+)B process_mem=(\d+)B'
$rows = @()

Get-Content -Path $ProxyLog | ForEach-Object {
    $line = $_
    $m = [regex]::Match($line, $pattern)
    if ($m.Success) {
        $rows += [pscustomobject]@{
            Active = [int]$m.Groups[1].Value
            UpBps = [uint64]$m.Groups[2].Value
            DownBps = [uint64]$m.Groups[3].Value
            TotalUp = [uint64]$m.Groups[4].Value
            TotalDown = [uint64]$m.Groups[5].Value
            TunnelMem = [uint64]$m.Groups[6].Value
            ProcessMem = [uint64]$m.Groups[7].Value
            Raw = $line
        }
    }
}

if ($rows.Count -eq 0) {
    throw "no traffic aggregate lines found in log"
}

$tailCount = [Math]::Min($IdleTailSeconds, $rows.Count)
$tail = $rows | Select-Object -Last $tailCount

$maxActive = ($rows | Measure-Object -Property Active -Maximum).Maximum
$maxUp = ($rows | Measure-Object -Property UpBps -Maximum).Maximum
$maxDown = ($rows | Measure-Object -Property DownBps -Maximum).Maximum
$maxTunnel = ($rows | Measure-Object -Property TunnelMem -Maximum).Maximum
$maxProc = ($rows | Measure-Object -Property ProcessMem -Maximum).Maximum

$tailActiveMax = ($tail | Measure-Object -Property Active -Maximum).Maximum
$tailUpMax = ($tail | Measure-Object -Property UpBps -Maximum).Maximum
$tailDownMax = ($tail | Measure-Object -Property DownBps -Maximum).Maximum
$tailTunnelMax = ($tail | Measure-Object -Property TunnelMem -Maximum).Maximum

$firstTailMem = $tail[0].ProcessMem
$lastTailMem = $tail[-1].ProcessMem
$tailMemDelta = [int64]$lastTailMem - [int64]$firstTailMem

Write-Host "=== Proxy Soak Summary ==="
Write-Host "samples           : $($rows.Count)"
Write-Host "max active        : $maxActive"
Write-Host "max up_Bps        : $maxUp"
Write-Host "max down_Bps      : $maxDown"
Write-Host "max tunnel_mem(B) : $maxTunnel"
Write-Host "max process_mem(B): $maxProc"
Write-Host "tail seconds      : $tailCount"
Write-Host "tail max active   : $tailActiveMax"
Write-Host "tail max up_Bps   : $tailUpMax"
Write-Host "tail max down_Bps : $tailDownMax"
Write-Host "tail max tunnel(B): $tailTunnelMax"
Write-Host "tail mem delta(B) : $tailMemDelta"

if ($tailActiveMax -eq 0 -and $tailUpMax -eq 0 -and $tailDownMax -eq 0) {
    Write-Host "idle convergence  : PASS"
} else {
    Write-Host "idle convergence  : WARN (active or throughput still non-zero in tail)"
}

if ($tailMemDelta -gt 0) {
    Write-Host "memory trend      : WARN (tail memory increasing)"
} else {
    Write-Host "memory trend      : PASS (tail memory stable or decreasing)"
}
