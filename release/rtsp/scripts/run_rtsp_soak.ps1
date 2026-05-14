param(
    [string]$BuildDir = "",
    [int]$DurationSec = 3600,
    [int]$Parallel = 4,
    [int]$MaxFailures = 1,
    [string]$Regex = "rtsp",
    [string]$OutDir = ".\\logs\\rtsp_soak"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-BuildDir {
    param([string]$InputDir)

    if ($InputDir -and $InputDir.Trim().Length -gt 0) {
        return [System.IO.Path]::GetFullPath($InputDir)
    }

    if ($env:YUAN_BUILD_DIR -and $env:YUAN_BUILD_DIR.Trim().Length -gt 0) {
        return [System.IO.Path]::GetFullPath($env:YUAN_BUILD_DIR)
    }

    $repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\\..\\..\\.."))
    $mingwDir = Join-Path $repoRoot "build-mingw"
    if (Test-Path -LiteralPath $mingwDir) {
        return $mingwDir
    }

    return (Join-Path $repoRoot "build")
}

function Ensure-Path {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "required path not found: $Path"
    }
}

$resolvedBuildDir = Resolve-BuildDir -InputDir $BuildDir
Ensure-Path -Path $resolvedBuildDir

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$driverLog = Join-Path $OutDir ("driver-" + $ts + ".log")
$jsonlPath = Join-Path $OutDir ("iterations-" + $ts + ".jsonl")
$summaryPath = Join-Path $OutDir ("summary-" + $ts + ".json")

"[$(Get-Date -Format o)] start rtsp soak" | Tee-Object -FilePath $driverLog -Append
"build_dir=$resolvedBuildDir duration_sec=$DurationSec parallel=$Parallel regex=$Regex max_failures=$MaxFailures" | Tee-Object -FilePath $driverLog -Append

$listOutput = & ctest --test-dir $resolvedBuildDir -N -R $Regex 2>&1
$listText = ($listOutput | Out-String)
$listText.TrimEnd() | Tee-Object -FilePath $driverLog -Append | Out-Null

$deadline = (Get-Date).AddSeconds($DurationSec)
$iteration = 0
$passCount = 0
$failCount = 0
$elapsedTotalMs = 0
$abortedByFailure = $false

while ((Get-Date) -lt $deadline) {
    ++$iteration
    $startedAt = Get-Date
    $sw = [System.Diagnostics.Stopwatch]::StartNew()

    & ctest --test-dir $resolvedBuildDir -R $Regex --output-on-failure -j $Parallel 2>&1 | Tee-Object -FilePath $driverLog -Append | Out-Null
    $exitCode = $LASTEXITCODE

    $sw.Stop()
    $endedAt = Get-Date
    $elapsedMs = [int][Math]::Round($sw.Elapsed.TotalMilliseconds)
    $elapsedTotalMs += $elapsedMs

    if ($exitCode -eq 0) {
        ++$passCount
    }
    else {
        ++$failCount
    }

    $entry = [ordered]@{
        iteration = $iteration
        started_at = $startedAt.ToString("o")
        ended_at = $endedAt.ToString("o")
        elapsed_ms = $elapsedMs
        exit_code = $exitCode
    }
    ($entry | ConvertTo-Json -Compress) | Add-Content -LiteralPath $jsonlPath

    "[$($endedAt.ToString("o"))] iteration=$iteration exit=$exitCode elapsed_ms=$elapsedMs pass=$passCount fail=$failCount" | Tee-Object -FilePath $driverLog -Append

    if ($failCount -ge $MaxFailures) {
        $abortedByFailure = $true
        "[$($endedAt.ToString("o"))] stop early because fail_count=$failCount reached max_failures=$MaxFailures" | Tee-Object -FilePath $driverLog -Append
        break
    }
}

$summary = [ordered]@{
    started_at = $ts
    build_dir = $resolvedBuildDir
    regex = $Regex
    duration_sec = $DurationSec
    parallel = $Parallel
    iterations = $iteration
    pass_count = $passCount
    fail_count = $failCount
    aborted_by_failure = $abortedByFailure
    avg_iteration_ms = if ($iteration -gt 0) { [int]($elapsedTotalMs / $iteration) } else { 0 }
    driver_log = [System.IO.Path]::GetFullPath($driverLog)
    iterations_jsonl = [System.IO.Path]::GetFullPath($jsonlPath)
}

$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath

"[$(Get-Date -Format o)] rtsp soak done" | Tee-Object -FilePath $driverLog -Append
"summary=$summaryPath" | Tee-Object -FilePath $driverLog -Append

if ($failCount -gt 0) {
    exit 1
}

exit 0
