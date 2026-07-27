<#
.SYNOPSIS
    Code-signs the built plugin DLL (and optionally the installer).

.DESCRIPTION
    Signs with signtool from the Windows SDK, using either:
      * -Thumbprint  : a certificate already in the CurrentUser\My store (recommended;
                       works with real CA-issued certs and with hardware tokens), or
      * -PfxPath     : a .pfx file (prompts for its password unless -PfxPassword given).

    Every signature is SHA-256 and RFC-3161 timestamped, so it remains valid after the
    certificate expires.

    NOTE: a self-signed certificate produces a technically valid signature but is NOT
    trusted by Windows or antivirus on other machines. For distribution you need a
    CA-issued code-signing certificate (see SIGNING.md).

.EXAMPLE
    # sign with a cert from the user store
    .\cmake\sign-plugin.ps1 -Thumbprint 1234ABCD... -Path build\RelWithDebInfo\win-capture-audio.dll
#>
[CmdletBinding()]
param(
    [string]$Thumbprint,
    [string]$PfxPath,
    [string]$PfxPassword,
    [string[]]$Path = @("build\RelWithDebInfo\win-capture-audio.dll"),
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = 'Stop'

if (-not $Thumbprint -and -not $PfxPath) {
    throw "Pass -Thumbprint <cert in CurrentUser\My> or -PfxPath <file.pfx>."
}

# Locate signtool in the newest installed Windows SDK
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$signtool = Get-ChildItem $kits -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if (-not $signtool) { throw "signtool.exe not found - install the Windows SDK." }

$certArgs = if ($Thumbprint) {
    @('/sha1', $Thumbprint)
} else {
    $a = @('/f', $PfxPath)
    if ($PfxPassword) { $a += @('/p', $PfxPassword) }
    $a
}

foreach ($file in $Path) {
    if (-not (Test-Path $file)) { throw "Not found: $file" }

    & $signtool sign /fd SHA256 /td SHA256 /tr $TimestampUrl @certArgs $file
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Timestamping failed; retrying without timestamp (signature will expire with the cert)."
        & $signtool sign /fd SHA256 @certArgs $file
        if ($LASTEXITCODE -ne 0) { throw "signtool failed for $file" }
    }

    # Verification is informational: with a self-signed cert it always reports an
    # untrusted root even though the signature itself is well-formed.
    $verify = & $signtool verify /pa /v $file 2>&1
    $verify | Select-String 'Successfully verified|Issued to' | ForEach-Object { $_.Line.Trim() }
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Signature present but not trusted by this machine (expected for self-signed certs; a CA-issued cert fixes this)."
    }
}

exit 0
