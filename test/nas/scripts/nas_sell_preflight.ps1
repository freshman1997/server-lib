$ErrorActionPreference = 'Stop'

function Invoke-Step {
    param(
        [string]$Name,
        [scriptblock]$Action
    )
    Write-Host "==> $Name"
    & $Action
    Write-Host "PASS: $Name"
}

function Require-Path {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "missing required path: $Path"
    }
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\.."))
$buildDir = if ($env:YUAN_BUILD_DIR) { $env:YUAN_BUILD_DIR } else { Join-Path $repoRoot 'build' }

Require-Path $buildDir

Invoke-Step -Name 'Build targeted NAS tests' -Action {
    & cmake --build $buildDir --target test_nas_service test_nas_webdav_integration test_nas_concurrency test_nas_smb_adapter --config Debug
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

Invoke-Step -Name 'Run targeted NAS tests' -Action {
    & ctest --test-dir $buildDir -R 'nas_service|nas_webdav_integration|nas_concurrency|nas_smb_adapter' --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'ctest failed' }
}

Invoke-Step -Name 'Verify admin console resource exists' -Action {
    $resource = Join-Path $repoRoot 'server\services\resources\nas_admin_console.html'
    Require-Path $resource
}

if (Get-Command smbclient -ErrorAction SilentlyContinue) {
    Write-Host 'INFO: smbclient is available. Run smbclient smoke suite in this environment.'
} else {
    Write-Host 'BLOCKED-ENV: smbclient is unavailable; external SMB interop remains pending.'
}

Write-Host 'Preflight complete.'
