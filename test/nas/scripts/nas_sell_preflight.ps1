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

function Test-ImpacketAvailable {
    $pythonCmd = Get-Command python -ErrorAction SilentlyContinue
    if (-not $pythonCmd) {
        return $false
    }
    & $pythonCmd.Source -c "import impacket.smbconnection" 2>$null
    return $LASTEXITCODE -eq 0
}

function Invoke-CtestRegex {
    param(
        [string]$BuildDir,
        [string]$Regex,
        [string]$SkipMessage
    )

    $listed = & ctest --test-dir $BuildDir -N -R $Regex 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "ctest -N failed for regex: $Regex"
    }

    if ($listed -notmatch 'Test\s+#\d+') {
        Write-Host "SKIP: $SkipMessage"
        return
    }

    & ctest --test-dir $BuildDir -R $Regex --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "ctest failed for regex: $Regex"
    }
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\.."))
$buildDir = if ($env:YUAN_BUILD_DIR) { $env:YUAN_BUILD_DIR } else { Join-Path $repoRoot 'build' }

Require-Path $buildDir

Invoke-Step -Name 'Build targeted NAS tests' -Action {
    & cmake --build $buildDir --target test_nas_service test_nas_webdav_integration test_nas_concurrency test_nas_smb_adapter test_smb_internal_client_smoke test_smb_nas_fixture --config Debug
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

Invoke-Step -Name 'Run targeted NAS tests' -Action {
    Invoke-CtestRegex -BuildDir $buildDir -Regex 'nas_service|nas_webdav_integration|nas_concurrency|nas_smb_adapter' -SkipMessage 'no NAS tests matched regex'
}

Invoke-Step -Name 'Verify admin console resource exists' -Action {
    $resource = Join-Path $repoRoot 'server\services\resources\nas_admin_console.html'
    Require-Path $resource
}

Invoke-Step -Name 'Run SMB interop smoke tests (fixture-backed when available)' -Action {
    Invoke-CtestRegex -BuildDir $buildDir -Regex 'smbclient_nas_smoke|smbclient_nas_smoke_signing_required|smb_internal_client_smoke|smb_internal_client_smoke_basic|smb_internal_client_smoke_ioctl' -SkipMessage 'no SMB smoke tests matched regex'
}

$hasSmbclient = [bool](Get-Command smbclient -ErrorAction SilentlyContinue)
$hasImpacket = Test-ImpacketAvailable
if ($hasSmbclient) {
    Write-Host 'INFO: smbclient is available.'
} elseif ($hasImpacket) {
    Write-Host 'INFO: smbclient is unavailable; Python impacket fallback is available.'
} else {
    Write-Host 'BLOCKED-ENV: neither smbclient nor Python impacket is available; SMB external interop remains pending.'
}

Write-Host 'Preflight complete.'
