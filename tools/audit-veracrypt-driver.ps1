#requires -Version 5.1
# Read-only feasibility evidence. Never installs, starts, stops or removes a driver.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string] $DriverPath)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$expectedHash = '1F0C6DB3559D1356C38A1486A967CD90DB5E6202E433FEA1DFE510DDB884FFB6'
$expectedSigner = '3847B761C2846DB72F61149176D09F9BB2D43E8A'
$item = Get-Item -LiteralPath $DriverPath -Force
if ($item.PSIsContainer) { throw 'Driver must be a regular file' }
if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse paths are not accepted' }
for ($node = $item.Directory; $null -ne $node; $node = $node.Parent) {
    if ($node.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse paths are not accepted' }
}
# Keep the reviewed file closed to concurrent writes/deletion while inspecting it.
$stream = [IO.File]::Open($item.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $digest = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
    finally { $sha.Dispose() }
    if ($digest -cne $expectedHash) { throw 'Unreviewed driver digest' }
    $signature = Get-AuthenticodeSignature -LiteralPath $item.FullName
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Thumbprint -cne $expectedSigner) {
        throw 'Driver signature or signer verification failed'
    }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($item.FullName).FileVersion
    if ($version -cne '1.26.29.3') { throw 'Unexpected driver file version' }
    [pscustomobject]@{
        Schema = 1
        Purpose = 'Read-only candidate audit; not installation or kernel-load proof'
        UpstreamRevision = 'b48e31f5b47da7d41025e3f0e02751675e15005a'
        Architecture = 'x64'
        Sha256 = $digest
        FileVersion = $version
        SignatureStatus = [string]$signature.Status
        SignerThumbprint = $signature.SignerCertificate.Thumbprint
        CleanMachineLoadVerified = $false
        SecureBootLoadVerified = $false
    } | ConvertTo-Json
} finally { $stream.Dispose() }
