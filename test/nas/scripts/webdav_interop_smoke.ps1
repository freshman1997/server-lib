$ErrorActionPreference = 'Stop'

function Exit-Skip {
    param([string]$Message)
    Write-Host "SKIP: $Message"
    exit 77
}

function Require-Tool {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        Exit-Skip "$Name is not installed"
    }
}

$target = if ($env:YUAN_WEBDAV_BASE_URL) { $env:YUAN_WEBDAV_BASE_URL.TrimEnd('/') } else { 'http://127.0.0.1:8081/dav/public' }
$user = if ($env:YUAN_WEBDAV_USER) { $env:YUAN_WEBDAV_USER } else { '' }
$pass = if ($env:YUAN_WEBDAV_PASSWORD) { $env:YUAN_WEBDAV_PASSWORD } else { '' }
$client = if ($env:YUAN_WEBDAV_CLIENT) { $env:YUAN_WEBDAV_CLIENT.ToLowerInvariant() } else { 'auto' }

if ([string]::IsNullOrWhiteSpace($user) -or [string]::IsNullOrWhiteSpace($pass)) {
    Exit-Skip 'set YUAN_WEBDAV_USER and YUAN_WEBDAV_PASSWORD'
}

$tmpRoot = Join-Path $env:TEMP ("yuan_webdav_smoke_" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmpRoot | Out-Null

try {
    if ($client -in @('auto', 'rclone')) {
        if (Get-Command rclone -ErrorAction SilentlyContinue) {
            Write-Host "Running rclone smoke against $target"
            $srcDir = Join-Path $tmpRoot 'src'
            $dstDir = Join-Path $tmpRoot 'dst'
            New-Item -ItemType Directory -Path $srcDir | Out-Null
            New-Item -ItemType Directory -Path $dstDir | Out-Null
            $payload = Join-Path $srcDir 'rclone-smoke.txt'
            'yuan webdav smoke via rclone' | Out-File -FilePath $payload -Encoding ascii

            $copyArgs = @(
                'copy',
                $srcDir,
                ("`:webdav,url=$target,user=$user,pass=$pass"),
                '--webdav-vendor=other'
            )
            $copyProc = Start-Process -FilePath 'rclone' -ArgumentList $copyArgs -PassThru -Wait -NoNewWindow
            if ($copyProc.ExitCode -ne 0) {
                Write-Host "FAIL: rclone upload failed with code $($copyProc.ExitCode)"
                exit 1
            }

            $pullArgs = @(
                'copy',
                ("`:webdav,url=$target,user=$user,pass=$pass"),
                $dstDir,
                '--webdav-vendor=other'
            )
            $pullProc = Start-Process -FilePath 'rclone' -ArgumentList $pullArgs -PassThru -Wait -NoNewWindow
            if ($pullProc.ExitCode -ne 0) {
                Write-Host "FAIL: rclone download failed with code $($pullProc.ExitCode)"
                exit 1
            }

            $downloaded = Join-Path $dstDir 'rclone-smoke.txt'
            if (-not (Test-Path $downloaded)) {
                Write-Host 'FAIL: rclone did not download expected file'
                exit 1
            }
            Write-Host 'PASS: rclone smoke completed'
            exit 0
        }
        if ($client -eq 'rclone') {
            Exit-Skip 'rclone is not installed'
        }
    }

    if ($client -in @('auto', 'cadaver')) {
        Require-Tool 'cadaver'
        Write-Host "Running cadaver smoke against $target"
        $cmdFile = Join-Path $tmpRoot 'cadaver.cmd'
        $localFile = Join-Path $tmpRoot 'cadaver-smoke.txt'
        'yuan webdav smoke via cadaver' | Out-File -FilePath $localFile -Encoding ascii
        @(
            "open $target",
            "user $user $pass",
            "put $localFile cadaver-smoke.txt",
            'ls',
            'delete cadaver-smoke.txt',
            'quit'
        ) | Set-Content -LiteralPath $cmdFile -Encoding ascii

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = 'cadaver'
        $psi.ArgumentList.Add('-r')
        $psi.ArgumentList.Add($cmdFile)
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.UseShellExecute = $false
        $proc = [System.Diagnostics.Process]::Start($psi)
        $stdout = $proc.StandardOutput.ReadToEnd()
        $stderr = $proc.StandardError.ReadToEnd()
        $proc.WaitForExit()
        if ($proc.ExitCode -ne 0) {
            Write-Host "FAIL: cadaver exited with code $($proc.ExitCode)"
            if ($stdout) { Write-Host $stdout }
            if ($stderr) { Write-Host $stderr }
            exit 1
        }

        Write-Host 'PASS: cadaver smoke completed'
        exit 0
    }

    Exit-Skip "unsupported YUAN_WEBDAV_CLIENT value: $client"
}
finally {
    if (Test-Path $tmpRoot) {
        Remove-Item -Path $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
