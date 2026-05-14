param(
    [string]$BuildDir = "",
    [string]$Regex = "rtsp_server|rtsp_state_matrix|rtsp_interop",
    [string]$ParallelLevels = "2,4,8",
    [int]$RoundsPerLevel = 3,
    [string]$OutDir = ".\\logs\\rtsp_concurrency"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $BuildDir) {
    if ($env:YUAN_BUILD_DIR) {
        $BuildDir = $env:YUAN_BUILD_DIR
    } elseif (Test-Path -LiteralPath "build-mingw") {
        $BuildDir = "build-mingw"
    } else {
        $BuildDir = "build"
    }
}

if (-not (Test-Path -LiteralPath $BuildDir)) {
    throw "build dir not found: $BuildDir"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $OutDir "concurrency-$ts.log"
$json = Join-Path $OutDir "concurrency-$ts.json"

$results = @()
$levels = @()
foreach ($part in ($ParallelLevels -split ',')) {
    $v = $part.Trim()
    if (-not $v) {
        continue
    }
    $levels += [int]$v
}
if ($levels.Count -eq 0) {
    throw "ParallelLevels is empty"
}

foreach ($p in $levels) {
    for ($r = 1; $r -le $RoundsPerLevel; $r++) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & ctest --test-dir $BuildDir -R $Regex --output-on-failure -j $p 2>&1 | Tee-Object -FilePath $log -Append | Out-Null
        $code = $LASTEXITCODE
        $sw.Stop()
        $item = [ordered]@{
            parallel = $p
            round = $r
            exit_code = $code
            elapsed_ms = [int][Math]::Round($sw.Elapsed.TotalMilliseconds)
        }
        $results += $item
        "[$(Get-Date -Format o)] parallel=$p round=$r exit=$code elapsed_ms=$($item.elapsed_ms)" | Tee-Object -FilePath $log -Append
        if ($code -ne 0) {
            $results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $json
            throw "concurrency run failed at parallel=$p round=$r"
        }
    }
}

$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $json
"report=$json" | Tee-Object -FilePath $log -Append
