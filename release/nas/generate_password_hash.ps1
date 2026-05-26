param(
    [string]$Password,
    [string]$Salt,
    [int]$Iterations = 210000,
    [int]$KeyBytes = 32
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

if (-not $Password) {
    $Password = Read-PlainPassword
}
if (-not $Salt) {
    $Salt = New-Salt
}
if ([Text.Encoding]::UTF8.GetByteCount($Salt) -lt 8) {
    throw "Salt must be at least 8 bytes. Omit -Salt to generate a safe random salt."
}
if ($Iterations -lt 100000) {
    throw "Iterations must be at least 100000 for production use."
}

$passwordBytes = [Text.Encoding]::UTF8.GetBytes($Password)
$saltBytes = [Text.Encoding]::UTF8.GetBytes($Salt)
$derive = [Security.Cryptography.Rfc2898DeriveBytes]::new(
    $passwordBytes,
    $saltBytes,
    $Iterations,
    [Security.Cryptography.HashAlgorithmName]::SHA256)
try {
    $digest = Convert-ToHex -Bytes ($derive.GetBytes($KeyBytes))
    'pbkdf2-sha256${0}${1}${2}' -f $Iterations, $Salt, $digest
} finally {
    $derive.Dispose()
}
