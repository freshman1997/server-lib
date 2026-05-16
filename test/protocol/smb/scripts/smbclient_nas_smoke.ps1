$ErrorActionPreference = 'Stop'

function Exit-Skip {
    param([string]$Message)
    Write-Host "SKIP: $Message"
    exit 77
}

function Read-Bytes {
    param([string]$Path)
    return [System.IO.File]::ReadAllBytes($Path)
}

$fixtureProc = $null
$fixtureRoot = $null
$tmpDir = $null

try {
    $smbclientCmd = Get-Command smbclient -ErrorAction SilentlyContinue
    $pythonCmd = Get-Command python -ErrorAction SilentlyContinue
    $usePythonClient = $false

    if (-not $smbclientCmd) {
        if ($pythonCmd) {
            & $pythonCmd.Source -c "import impacket.smbconnection" 2>$null
            if ($LASTEXITCODE -eq 0) {
                $usePythonClient = $true
            }
        }
        if (-not $usePythonClient) {
            Exit-Skip "neither smbclient nor Python impacket is installed"
        }
    }

    $smbHost = if ($env:YUAN_SMB_HOST) { $env:YUAN_SMB_HOST } else { '127.0.0.1' }
    $smbPort = if ($env:YUAN_SMB_PORT) { $env:YUAN_SMB_PORT } else { '445' }
    $smbDomain = if ($env:YUAN_SMB_DOMAIN) { $env:YUAN_SMB_DOMAIN } else { 'WORKGROUP' }
    $smbSigning = if ($env:YUAN_SMBCLIENT_SIGNING) { $env:YUAN_SMBCLIENT_SIGNING.ToLowerInvariant() } else { 'default' }
    $smbShare = $env:YUAN_SMB_SHARE
    $smbUser = $env:YUAN_SMB_USER
    $smbPassword = $env:YUAN_SMB_PASSWORD

    $signingArgs = @()
    switch ($smbSigning) {
        'default' { }
        'auto' { }
        'required' {
            if ($usePythonClient) {
                $signingArgs = @()
            }
            else {
                $helpText = (& smbclient --help 2>&1 | Out-String)
                if ($helpText -match '--signing') {
                    $signingArgs = @('--signing=required')
                }
                elseif ($helpText -match '--client-protection') {
                    $signingArgs = @('--client-protection=sign')
                }
                else {
                    Exit-Skip 'smbclient does not support required-signing options'
                }
            }
        }
        default {
            Exit-Skip "unsupported YUAN_SMBCLIENT_SIGNING value: $smbSigning"
        }
    }

    if ($env:YUAN_SMB_USE_FIXTURE -eq '1') {
        if (-not $env:YUAN_SMB_FIXTURE_BIN) {
            Exit-Skip "set YUAN_SMB_FIXTURE_BIN when YUAN_SMB_USE_FIXTURE=1"
        }
        if (-not (Test-Path $env:YUAN_SMB_FIXTURE_BIN)) {
            Exit-Skip "fixture binary not found: $($env:YUAN_SMB_FIXTURE_BIN)"
        }

        if (-not $smbShare) { $smbShare = 'public' }
        if (-not $smbUser) { $smbUser = 'fixture' }
        if (-not $smbPassword) { $smbPassword = 'fixture-secret' }

        $fixtureRoot = Join-Path $env:TEMP ("yuan_smb_fixture_" + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
        $readyFile = Join-Path $fixtureRoot 'ready.flag'
        $fixtureOutLog = Join-Path $fixtureRoot 'fixture.out.log'
        $fixtureErrLog = Join-Path $fixtureRoot 'fixture.err.log'

        $fixtureArgs = @($fixtureRoot, $smbShare, $smbUser, $smbPassword, $smbPort, $readyFile)
        $fixtureProc = Start-Process -FilePath $env:YUAN_SMB_FIXTURE_BIN `
            -ArgumentList $fixtureArgs `
            -PassThru `
            -NoNewWindow `
            -RedirectStandardOutput $fixtureOutLog `
            -RedirectStandardError $fixtureErrLog

        $ready = $false
        for ($i = 0; $i -lt 50; $i++) {
            if (Test-Path $readyFile) {
                $ready = $true
                break
            }
            if ($fixtureProc.HasExited) {
                Write-Host "FAIL: SMB fixture exited early"
                if (Test-Path $fixtureOutLog) { Get-Content $fixtureOutLog }
                if (Test-Path $fixtureErrLog) { Get-Content $fixtureErrLog }
                exit 1
            }
            Start-Sleep -Milliseconds 100
        }

        if (-not $ready) {
            Write-Host "FAIL: SMB fixture did not become ready"
            if (Test-Path $fixtureOutLog) { Get-Content $fixtureOutLog }
            if (Test-Path $fixtureErrLog) { Get-Content $fixtureErrLog }
            exit 1
        }
    }
    elseif (-not $smbShare -or -not $smbUser -or -not $smbPassword) {
        Exit-Skip "set YUAN_SMB_SHARE, YUAN_SMB_USER, and YUAN_SMB_PASSWORD, or enable YUAN_SMB_USE_FIXTURE=1"
    }

    $tmpDir = Join-Path $env:TEMP ("yuan_smb_smoke_" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmpDir | Out-Null

    $localFile = Join-Path $tmpDir 'yuan_smb_smoke.txt'
    $downloadedFile = Join-Path $tmpDir 'yuan_smb_smoke.downloaded.txt'
    $remoteFile = "yuan_smb_smoke_$PID.txt"
    $renamedFile = "yuan_smb_smoke_renamed_$PID.txt"
    $smbLog = Join-Path $tmpDir 'smbclient.log'

    "yuan smb smoke $PID" | Out-File -FilePath $localFile -Encoding ascii

    if ($usePythonClient) {
        $pythonClient = Join-Path $PSScriptRoot 'smbclient_nas_smoke.py'
        $pyArgs = @(
            $pythonClient,
            '--host', $smbHost,
            '--port', $smbPort,
            '--share', $smbShare,
            '--domain', $smbDomain,
            '--user', $smbUser,
            '--password', $smbPassword,
            '--signing', $smbSigning,
            '--local-file', $localFile,
            '--downloaded-file', $downloadedFile,
            '--remote-file', $remoteFile,
            '--renamed-file', $renamedFile
        )

        $smbErrLog = Join-Path $tmpDir 'impacket.err.log'
        $pyProc = Start-Process -FilePath $pythonCmd.Source `
            -ArgumentList $pyArgs `
            -Wait `
            -PassThru `
            -NoNewWindow `
            -RedirectStandardOutput $smbLog `
            -RedirectStandardError $smbErrLog

        if ($pyProc.ExitCode -ne 0) {
            Write-Host "FAIL: impacket SMB client exited with code $($pyProc.ExitCode)"
            if (Test-Path $smbLog) { Get-Content $smbLog }
            if (Test-Path $smbErrLog) { Get-Content $smbErrLog }
            if (Test-Path $fixtureOutLog) { Get-Content $fixtureOutLog }
            if (Test-Path $fixtureErrLog) { Get-Content $fixtureErrLog }
            exit 1
        }

        if (Test-Path $smbLog) { Get-Content $smbLog }
        exit 0
    }

    $commands = "put `"$localFile`" `"$remoteFile`"; get `"$remoteFile`" `"$downloadedFile`"; rename `"$remoteFile`" `"$renamedFile`"; del `"$renamedFile`"; ls"
    $smbArgs = @(
        "//$smbHost/$smbShare",
        '-p', $smbPort,
        '-W', $smbDomain,
        '-U', "$smbUser%$smbPassword",
        '-m', 'SMB3'
    )
    $smbArgs += $signingArgs
    $smbArgs += @('-c', $commands)

    $smbProc = Start-Process -FilePath 'smbclient' `
        -ArgumentList $smbArgs `
        -Wait `
        -PassThru `
        -NoNewWindow `
        -RedirectStandardOutput $smbLog `
        -RedirectStandardError $smbLog

    if ($smbProc.ExitCode -ne 0) {
        Write-Host "FAIL: smbclient exited with code $($smbProc.ExitCode)"
        if (Test-Path $smbLog) { Get-Content $smbLog }
        exit 1
    }

    $src = Read-Bytes -Path $localFile
    $dst = Read-Bytes -Path $downloadedFile
    if ($src.Length -ne $dst.Length) {
        Write-Host "FAIL: downloaded file length mismatch"
        exit 1
    }
    for ($i = 0; $i -lt $src.Length; $i++) {
        if ($src[$i] -ne $dst[$i]) {
            Write-Host "FAIL: downloaded file content mismatch at byte $i"
            exit 1
        }
    }

    Write-Host 'PASS: smbclient NAS smoke completed'
    exit 0
}
finally {
    if ($fixtureProc -and -not $fixtureProc.HasExited) {
        Stop-Process -Id $fixtureProc.Id -Force -ErrorAction SilentlyContinue
    }
    if ($tmpDir -and (Test-Path $tmpDir)) {
        Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($fixtureRoot -and (Test-Path $fixtureRoot)) {
        Remove-Item -Path $fixtureRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
