param(
    [string]$BuildDir = "",
    [string]$Regex = "rtsp|rtcp",
    [string]$OutDir = ".\\logs\\rtsp_gate"
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

$resolvedBuildDir = Resolve-BuildDir -InputDir $BuildDir
if (-not (Test-Path -LiteralPath $resolvedBuildDir)) {
    throw "build dir not found: $resolvedBuildDir"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$logFile = Join-Path $OutDir ("rtsp-gate-" + $ts + ".log")

"[$(Get-Date -Format o)] build dir=$resolvedBuildDir regex=$Regex" | Tee-Object -FilePath $logFile -Append

& ctest --test-dir $resolvedBuildDir -R $Regex --output-on-failure 2>&1 | Tee-Object -FilePath $logFile -Append | Out-Null
$code = $LASTEXITCODE

if ($code -ne 0) {
    "[$(Get-Date -Format o)] gate failed code=$code log=$logFile" | Tee-Object -FilePath $logFile -Append
    exit $code
}

"[$(Get-Date -Format o)] gate passed log=$logFile" | Tee-Object -FilePath $logFile -Append
exit 0
