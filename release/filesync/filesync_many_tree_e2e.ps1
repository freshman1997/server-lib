param(
    [Parameter(Mandatory = $true)]
    [string]$ServerExe,

    [Parameter(Mandatory = $true)]
    [string]$ClientExe
)

$ErrorActionPreference = 'Stop'

function Get-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse('127.0.0.1'), 0)
    $listener.Start()
    try {
        return ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Wait-Until {
    param(
        [Parameter(Mandatory = $true)]
        [scriptblock]$Condition,

        [int]$TimeoutSeconds = 90,
        [int]$SleepMilliseconds = 250
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (& $Condition) {
            return $true
        }
        Start-Sleep -Milliseconds $SleepMilliseconds
    }
    return $false
}

function Stop-IfRunning {
    param($Process)
    if ($Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    }
}

function Dump-LogTail {
    param([string]$Label, [string]$Path)
    Write-Output "--- $Label ---"
    if (Test-Path -LiteralPath $Path) {
        Get-Content -LiteralPath $Path -Tail 120
    }
}

if (!(Test-Path -LiteralPath $ServerExe)) {
    throw "server executable not found: $ServerExe"
}
if (!(Test-Path -LiteralPath $ClientExe)) {
    throw "client executable not found: $ClientExe"
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("filesync-many-" + [System.Guid]::NewGuid().ToString('N'))
$serverDir = Join-Path $tmp 'server'
$clientDir = Join-Path $tmp 'client'
$serverCfg = Join-Path $tmp 'server.json'
$clientCfg = Join-Path $tmp 'client.json'
$serverLog = Join-Path $tmp 'server.log'
$clientLog = Join-Path $tmp 'client.log'
$serverErr = Join-Path $tmp 'server.err'
$clientErr = Join-Path $tmp 'client.err'

$server = $null
$client = $null
$success = $false

try {
    New-Item -ItemType Directory -Force -Path $serverDir, $clientDir | Out-Null

    $dirCount = 60
    $filesPerDir = 20
    $totalFiles = 0
    for ($d = 0; $d -lt $dirCount; $d++) {
        $leaf = Join-Path $clientDir "dir_$d\nested\leaf"
        New-Item -ItemType Directory -Force -Path $leaf | Out-Null
        New-Item -ItemType Directory -Force -Path (Join-Path $clientDir "empty_$d\child") | Out-Null
        for ($f = 0; $f -lt $filesPerDir; $f++) {
            $path = Join-Path $leaf "file_$f.txt"
            Set-Content -LiteralPath $path -Value ("payload $d/$f " + ('x' * 512)) -NoNewline
            $totalFiles++
        }
    }

    Set-Content -LiteralPath (Join-Path $serverDir 'server_only.txt') -Value 'from-server' -NoNewline

    $serverPort = Get-FreeTcpPort
    do {
        $clientPort = Get-FreeTcpPort
    } while ($clientPort -eq $serverPort)

    $serverJson = @{
        listen_host = '127.0.0.1'
        listen_port = $serverPort
        peer_host = ''
        peer_port = $clientPort
        token = 'test-token'
        conflict_strategy = 'newer_wins'
        sync_deletes = $true
        scan_interval_ms = 10
        chunk_size = 4096
        include_extensions = @()
        include_patterns = @()
        exclude_patterns = @()
        paths = @(@{ local = $serverDir.Replace('\', '/'); remote_prefix = 'work' })
    } | ConvertTo-Json -Depth 6 -Compress

    $clientJson = @{
        listen_host = '127.0.0.1'
        listen_port = $clientPort
        peer_host = '127.0.0.1'
        peer_port = $serverPort
        token = 'test-token'
        conflict_strategy = 'newer_wins'
        sync_deletes = $true
        scan_interval_ms = 10
        chunk_size = 4096
        include_extensions = @()
        include_patterns = @()
        exclude_patterns = @()
        paths = @(@{ local = $clientDir.Replace('\', '/'); remote_prefix = 'work' })
    } | ConvertTo-Json -Depth 6 -Compress

    Set-Content -LiteralPath $serverCfg -Value $serverJson -NoNewline
    Set-Content -LiteralPath $clientCfg -Value $clientJson -NoNewline

    $server = Start-Process -FilePath $ServerExe -ArgumentList @($serverCfg) `
        -RedirectStandardOutput $serverLog -RedirectStandardError $serverErr `
        -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 300
    $client = Start-Process -FilePath $ClientExe -ArgumentList @($clientCfg) `
        -RedirectStandardOutput $clientLog -RedirectStandardError $clientErr `
        -WindowStyle Hidden -PassThru

    $initialSynced = Wait-Until -TimeoutSeconds 90 -Condition {
        if ($client.HasExited -or $server.HasExited) {
            return $false
        }
        $syncedFiles = @(Get-ChildItem -LiteralPath $serverDir -Recurse -File -Filter 'file_*.txt' -ErrorAction SilentlyContinue).Count
        return $syncedFiles -ge $totalFiles -and (Test-Path -LiteralPath (Join-Path $clientDir 'server_only.txt'))
    }
    if (!$initialSynced) {
        throw "initial many-tree sync did not complete"
    }

    $emptyDirs = @(Get-ChildItem -LiteralPath $serverDir -Recurse -Directory -Filter 'child' -ErrorAction SilentlyContinue).Count
    if ($emptyDirs -lt $dirCount) {
        throw "empty directory sync incomplete: expected at least $dirCount, got $emptyDirs"
    }

    $sampleSource = Join-Path $clientDir 'dir_42\nested\leaf\file_7.txt'
    $sampleTarget = Join-Path $serverDir 'dir_42\nested\leaf\file_7.txt'
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $sampleSource).Hash -ne
        (Get-FileHash -Algorithm SHA256 -LiteralPath $sampleTarget).Hash) {
        throw "sample file hash mismatch after many-tree sync"
    }

    $laterPath = Join-Path $serverDir 'server_later\nested\after.txt'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $laterPath) | Out-Null
    Set-Content -LiteralPath $laterPath -Value 'server-later' -NoNewline

    $laterPulled = Wait-Until -TimeoutSeconds 60 -Condition {
        if ($client.HasExited -or $server.HasExited) {
            return $false
        }
        return Test-Path -LiteralPath (Join-Path $clientDir 'server_later\nested\after.txt')
    }
    if (!$laterPulled) {
        throw "client did not pull server-side change when local manifest was unchanged"
    }

    if ($client.HasExited) {
        throw "client process exited unexpectedly"
    }
    if ($server.HasExited) {
        throw "server process exited unexpectedly"
    }

    Write-Output "filesync many-tree e2e OK files=$totalFiles empty_dirs=$emptyDirs tmp=$tmp"
    $success = $true
} catch {
    Write-Output "filesync many-tree e2e FAILED tmp=$tmp"
    Write-Output $_.Exception.Message
    if ($client -and $client.HasExited) {
        Write-Output "client exit code: $($client.ExitCode)"
    }
    if ($server -and $server.HasExited) {
        Write-Output "server exit code: $($server.ExitCode)"
    }
    Dump-LogTail 'client.err' $clientErr
    Dump-LogTail 'server.err' $serverErr
    Dump-LogTail 'client.log' $clientLog
    Dump-LogTail 'server.log' $serverLog
    exit 1
} finally {
    Stop-IfRunning $client
    Stop-IfRunning $server

    $tempRoot = [System.IO.Path]::GetTempPath()
    $tmpName = Split-Path -Leaf $tmp
    if ($success -and $tmp.StartsWith($tempRoot, [System.StringComparison]::OrdinalIgnoreCase) -and
        $tmpName.StartsWith('filesync-many-', [System.StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
