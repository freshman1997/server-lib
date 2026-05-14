$ErrorActionPreference = "Stop"

function Invoke-Step {
    param(
        [string]$Name,
        [scriptblock]$Action
    )
    "==> $Name"
    & $Action
    "PASS: $Name"
}

function Resolve-BuildDir {
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

$buildDir = Resolve-BuildDir
if (-not (Test-Path -LiteralPath $buildDir)) {
    throw "missing build dir: $buildDir"
}

Invoke-Step -Name "Build RTSP/RTCP test binaries" -Action {
    & cmake --build $buildDir --target test_rtsp test_rtsp_server test_rtsp_interop test_rtcp test_rtcp_session test_rtcp_loopback
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

Invoke-Step -Name "Run RTSP/RTCP gate" -Action {
    & pwsh -File (Join-Path $PSScriptRoot "run_rtsp_gate.ps1") -BuildDir $buildDir -Regex "rtsp|rtcp"
    if ($LASTEXITCODE -ne 0) { throw "gate failed" }
}

"RTSP preflight complete."
