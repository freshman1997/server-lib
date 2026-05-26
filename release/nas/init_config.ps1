param(
    [string]$TemplatePath = (Join-Path $PSScriptRoot "config.json"),
    [string]$OutputPath = (Join-Path $PSScriptRoot "config.production.json"),
    [string]$AdminUser = "admin",
    [string]$AdminPassword,
    [string]$ShareRoot = $(if ($env:YUAN_NAS_SHARE_ROOT) { $env:YUAN_NAS_SHARE_ROOT } else { "C:/yuan/nas/public" }),
    [string]$RedisHost = $(if ($env:REDIS_HOST) { $env:REDIS_HOST } else { "127.0.0.1" }),
    [int]$RedisPort = $(if ($env:REDIS_PORT) { [int]$env:REDIS_PORT } else { 6379 }),
    [int]$Port = $(if ($env:NAS_PORT) { [int]$env:NAS_PORT } else { 8080 })
)

$ErrorActionPreference = "Stop"

function Read-PlainPassword {
    $secure = Read-Host "NAS admin password" -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
    } finally {
        if ($ptr -ne [IntPtr]::Zero) {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
        }
    }
}

function New-Salt {
    $bytes = New-Object byte[] 16
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($bytes)
        return (($bytes | ForEach-Object { $_.ToString("x2") }) -join "")
    } finally {
        $rng.Dispose()
    }
}

function Convert-ToHex {
    param([byte[]]$Bytes)
    return (($Bytes | ForEach-Object { $_.ToString("x2") }) -join "")
}

function New-PasswordHash {
    param([string]$Password)
    $iterations = 210000
    $salt = New-Salt
    $derive = [Security.Cryptography.Rfc2898DeriveBytes]::new(
        [Text.Encoding]::UTF8.GetBytes($Password),
        [Text.Encoding]::UTF8.GetBytes($salt),
        $iterations,
        [Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        $digest = Convert-ToHex -Bytes ($derive.GetBytes(32))
        return 'pbkdf2-sha256${0}${1}${2}' -f $iterations, $salt, $digest
    } finally {
        $derive.Dispose()
    }
}

if (-not (Test-Path -LiteralPath $TemplatePath)) {
    throw "template config not found: $TemplatePath"
}
if (-not $AdminPassword) {
    $AdminPassword = Read-PlainPassword
}
if (-not $AdminPassword) {
    throw "admin password is required"
}

$config = Get-Content -LiteralPath $TemplatePath -Raw | ConvertFrom-Json
$config.production_mode = $true
$config.port = $Port
$config.nas.redis.enabled = $true
$config.nas.redis.host = $RedisHost
$config.nas.redis.port = $RedisPort
$config.nas.users[0].username = $AdminUser
$config.nas.users[0].password_hash = New-PasswordHash -Password $AdminPassword
$config.nas.users[0].enabled = $true
$config.nas.users[0].admin = $true
$config.nas.shares[0].windows_root_path = ($ShareRoot -replace "\\", "/")
$config.nas.shares[0].root_env = "YUAN_NAS_SHARE_ROOT"

$shareDir = New-Item -ItemType Directory -Force -Path $ShareRoot
$outputDir = Split-Path -Parent $OutputPath
if ($outputDir) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}
$logDir = Join-Path $outputDir "logs"
if ($logDir) {
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
}

$config | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Host "wrote config: $OutputPath"
Write-Host "share root: $($shareDir.FullName)"
Write-Host "next:"
Write-Host "  `$env:YUAN_NAS_CONFIG = '$OutputPath'"
Write-Host "  `$env:YUAN_NAS_ADMIN_USER = '$AdminUser'"
Write-Host "  `$env:YUAN_NAS_ADMIN_PASSWORD = '<password you entered>'"
