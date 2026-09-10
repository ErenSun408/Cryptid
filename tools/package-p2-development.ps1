#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildDirectory,

    [Parameter(Mandatory = $true)]
    [string] $VendorDll,

    [Parameter(Mandatory = $true)]
    [string] $VeraCryptSourceDirectory,

    [Parameter(Mandatory = $true)]
    [string] $VeraCryptInstaller,

    [Parameter(Mandatory = $true)]
    [string] $WebViewInstaller,

    [string] $DestinationRoot,

    [string] $PackageName
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ExpectedVendorSha256 = '735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E'
$ExpectedFormatSha256 = 'E411BB9552ACA94B1285570E45C3D77D44652519309B7D33EC0A4EF99AF28AB3'
$ExpectedVeraCryptInstallerSha256 = 'DCA9DDEC7934FC0B1C25AC8984DB7D504BFE9F37C21861AC91C67A98ECD3964B'
$ExpectedVeraCryptSignerThumbprint = '86E1D426731E79117452F090188A828426B29B5F'
$ExpectedVeraCryptRevision = 'b48e31f5b47da7d41025e3f0e02751675e15005a'

function Resolve-ExistingFile([string] $Path, [string] $Description) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if (-not $item.PSIsContainer -and
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)) {
        return $item.FullName
    }
    throw "$Description must be a regular non-reparse file: $Path"
}

function Resolve-ExistingDirectory([string] $Path, [string] $Description) {
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item.PSIsContainer -and
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)) {
        return $item.FullName.TrimEnd('\')
    }
    throw "$Description must be a non-reparse directory: $Path"
}

function Require-Sha256([string] $Path, [string] $Expected, [string] $Description) {
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ne $Expected) {
        throw "$Description SHA-256 mismatch. Expected $Expected, got $actual"
    }
    return $actual
}

$RepositoryRoot = Resolve-ExistingDirectory (Join-Path $PSScriptRoot '..') 'repository root'
$ResolvedBuild = Resolve-ExistingDirectory $BuildDirectory 'package build directory'
$ResolvedVendorDll = Resolve-ExistingFile $VendorDll 'ViKey vendor DLL'
$ResolvedVeraCryptSource = Resolve-ExistingDirectory $VeraCryptSourceDirectory 'VeraCrypt source directory'
$ResolvedVeraCryptInstaller = Resolve-ExistingFile $VeraCryptInstaller 'VeraCrypt installer'
$ResolvedWebViewInstaller = Resolve-ExistingFile $WebViewInstaller 'WebView2 offline installer'

if ([IO.Path]::GetFileName($ResolvedVendorDll) -cne 'ViKey.dll') {
    throw 'The vendor runtime must have the exact filename ViKey.dll'
}
$CachePath = Resolve-ExistingFile (Join-Path $ResolvedBuild 'CMakeCache.txt') 'CMake cache'
$CacheText = Get-Content -LiteralPath $CachePath -Raw
if ($CacheText -notmatch '(?m)^CMAKE_BUILD_TYPE:[^=]+=Release\s*$' -or
    $CacheText -notmatch '(?m)^CMAKE_MSVC_RUNTIME_LIBRARY:[^=]+=MultiThreaded\s*$' -or
    $CacheText -notmatch '(?m)^BUILD_TESTING:[^=]+=OFF\s*$') {
    throw 'The package build must use package-release (Release, static MSVC runtime, tests off)'
}

$RequiredBuildFiles = @(
    'usb_vault_vikey_p2_development_app.exe',
    'usb_vault_vikey_development_credentials_config.exe',
    'usb_vault_vikey_helper.exe',
    'usb_vault_volume_creator.exe',
    'VeraCryptFormat.dll',
    'card_session.html',
    'card_session.js'
)
$ResolvedBuildFiles = @{}
foreach ($name in $RequiredBuildFiles) {
    $ResolvedBuildFiles[$name] = Resolve-ExistingFile (Join-Path $ResolvedBuild $name) "build artifact $name"
}

$VendorSha256 = Require-Sha256 $ResolvedVendorDll $ExpectedVendorSha256 'ViKey.dll'
$FormatSha256 = Require-Sha256 $ResolvedBuildFiles['VeraCryptFormat.dll'] $ExpectedFormatSha256 'VeraCryptFormat.dll'
$VeraCryptInstallerSha256 = Require-Sha256 $ResolvedVeraCryptInstaller $ExpectedVeraCryptInstallerSha256 'VeraCrypt installer'
$InstallerPolicy = Get-Content -LiteralPath (Join-Path $RepositoryRoot 'tools\vikey_development_credentials_config\installers.h') -Raw
if ($InstallerPolicy -notmatch 'kWebViewInstallerDigest\[\]\s*=\s*L"([A-F0-9]{64})"') {
    throw 'Cannot resolve reviewed WebView2 installer digest'
}
$WebViewInstallerSha256 = Require-Sha256 $ResolvedWebViewInstaller $Matches[1] 'WebView2 offline installer'
$WebViewSignature = Get-AuthenticodeSignature -LiteralPath $ResolvedWebViewInstaller
if ($WebViewSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
    $WebViewSignature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
    throw 'WebView2 installer must have a valid Microsoft Authenticode signature'
}
$HelperSha256 = (Get-FileHash -LiteralPath $ResolvedBuildFiles['usb_vault_vikey_helper.exe'] -Algorithm SHA256).Hash

$VeraCryptInstallerVersion = (Get-Item -LiteralPath $ResolvedVeraCryptInstaller).VersionInfo
if ($VeraCryptInstallerVersion.ProductName -cne 'VeraCrypt' -or
    $VeraCryptInstallerVersion.ProductVersion -cne '1.26.29' -or
    $VeraCryptInstallerVersion.OriginalFilename -cne 'VeraCrypt Setup.exe') {
    throw 'The VeraCrypt installer version metadata is not the reviewed 1.26.29 setup executable'
}
$VeraCryptInstallerSignature = Get-AuthenticodeSignature -LiteralPath $ResolvedVeraCryptInstaller
if ($VeraCryptInstallerSignature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
    $null -eq $VeraCryptInstallerSignature.SignerCertificate -or
    $VeraCryptInstallerSignature.SignerCertificate.Thumbprint -ne $ExpectedVeraCryptSignerThumbprint) {
    throw "The VeraCrypt installer Authenticode identity is not the reviewed release: $($VeraCryptInstallerSignature.Status)"
}

foreach ($asset in @('card_session.html', 'card_session.js')) {
    $sourceAsset = Resolve-ExistingFile (Join-Path $RepositoryRoot "ui\$asset") "source UI asset $asset"
    $sourceHash = (Get-FileHash -LiteralPath $sourceAsset -Algorithm SHA256).Hash
    $builtHash = (Get-FileHash -LiteralPath $ResolvedBuildFiles[$asset] -Algorithm SHA256).Hash
    if ($sourceHash -ne $builtHash) {
        throw "Built UI asset is stale: $asset"
    }
}

$GitRevision = (& git -C $RepositoryRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $GitRevision -notmatch '^[0-9a-f]{40}$') {
    throw 'Cannot resolve the source Git revision'
}
$WorkingTreeState = if ((& git -C $RepositoryRoot status --porcelain --untracked-files=no)) { 'dirty' } else { 'clean' }

$VeraCryptRevision = (& git -C $ResolvedVeraCryptSource rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $VeraCryptRevision -ne $ExpectedVeraCryptRevision) {
    throw "VeraCrypt source revision mismatch: $VeraCryptRevision"
}
$VeraCryptLicense = Resolve-ExistingFile (Join-Path $ResolvedVeraCryptSource 'License.txt') 'VeraCrypt license'
$VeraCryptNotice = Resolve-ExistingFile (Join-Path $ResolvedVeraCryptSource 'src\NOTICE') 'VeraCrypt NOTICE'

if ([string]::IsNullOrWhiteSpace($DestinationRoot)) {
    $DestinationRoot = Join-Path $RepositoryRoot 'out\packages'
}
if (-not (Test-Path -LiteralPath $DestinationRoot)) {
    $null = New-Item -ItemType Directory -Path $DestinationRoot
}
$ResolvedDestination = Resolve-ExistingDirectory $DestinationRoot 'package destination'

if ([string]::IsNullOrWhiteSpace($PackageName)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $PackageName = "usb-vault-p2-development-test-$stamp"
}
if ($PackageName -notmatch '^[A-Za-z0-9._-]+$') {
    throw 'PackageName may contain only ASCII letters, digits, dot, underscore, and hyphen'
}
if ($PackageName -eq '.' -or $PackageName -eq '..') {
    throw 'PackageName cannot be a relative directory marker'
}

$PackageDirectory = [IO.Path]::GetFullPath((Join-Path $ResolvedDestination $PackageName))
$PackageParent = [IO.Path]::GetDirectoryName($PackageDirectory).TrimEnd('\')
if (-not $PackageParent.Equals(
        $ResolvedDestination.TrimEnd('\'),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Package output escaped the requested destination'
}
$ZipPath = "$PackageDirectory.zip"
if ((Test-Path -LiteralPath $PackageDirectory) -or
    (Test-Path -LiteralPath $ZipPath)) {
    throw "Package output already exists: $PackageDirectory"
}

$null = New-Item -ItemType Directory -Path $PackageDirectory
$VendorDirectory = New-Item -ItemType Directory -Path (Join-Path $PackageDirectory 'vendor')
$PrerequisitesDirectory = New-Item -ItemType Directory -Path (Join-Path $PackageDirectory 'prerequisites')
$LicenseDirectory = New-Item -ItemType Directory -Path (Join-Path $PackageDirectory 'licenses')
$Argon2LicenseDirectory = New-Item -ItemType Directory -Path (Join-Path $LicenseDirectory.FullName 'argon2')
$VeraCryptLicenseDirectory = New-Item -ItemType Directory -Path (Join-Path $LicenseDirectory.FullName 'veracrypt')

foreach ($name in $RequiredBuildFiles) {
    Copy-Item -LiteralPath $ResolvedBuildFiles[$name] -Destination (Join-Path $PackageDirectory $name)
}
Copy-Item -LiteralPath $ResolvedVendorDll -Destination (Join-Path $VendorDirectory.FullName 'ViKey.dll')
Copy-Item -LiteralPath $ResolvedVeraCryptInstaller -Destination (Join-Path $PrerequisitesDirectory.FullName 'VeraCrypt Setup 1.26.29.exe')
Copy-Item -LiteralPath $ResolvedWebViewInstaller -Destination (Join-Path $PrerequisitesDirectory.FullName 'MicrosoftEdgeWebView2RuntimeInstallerX64.exe')
Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'packaging\p2-development\THIRD-PARTY-NOTICES.txt') -Destination (Join-Path $LicenseDirectory.FullName 'THIRD-PARTY-NOTICES.txt')
Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'third_party\argon2\LICENSE') -Destination (Join-Path $Argon2LicenseDirectory.FullName 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'third_party\veracrypt_format\veracrypt-1.26.29-usb-vault.patch') -Destination $VeraCryptLicenseDirectory.FullName
Copy-Item -LiteralPath $VeraCryptLicense -Destination (Join-Path $VeraCryptLicenseDirectory.FullName 'License.txt')
Copy-Item -LiteralPath $VeraCryptNotice -Destination (Join-Path $VeraCryptLicenseDirectory.FullName 'NOTICE')


$RevisionText = @(
    "USB Vault source revision: $GitRevision",
    "USB Vault working tree at packaging: $WorkingTreeState",
    "VeraCrypt source revision: $VeraCryptRevision",
    "VeraCrypt installer SHA-256: $VeraCryptInstallerSha256",
    "WebView2 offline installer SHA-256: $WebViewInstallerSha256",
    "VeraCrypt installer signer: $($VeraCryptInstallerSignature.SignerCertificate.Subject)",
    "ViKey.dll SHA-256: $VendorSha256",
    "VeraCryptFormat.dll SHA-256: $FormatSha256",
    "usb_vault_vikey_helper.exe SHA-256: $HelperSha256"
) -join [Environment]::NewLine
Set-Content -LiteralPath "$PackageDirectory.build-identity.txt" -Value $RevisionText -Encoding UTF8

$ManifestLines = Get-ChildItem -LiteralPath $PackageDirectory -Recurse -File |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($PackageDirectory.Length + 1).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        "$hash  $relative"
    }
Set-Content -LiteralPath "$PackageDirectory.sha256sums.txt" -Value $ManifestLines -Encoding UTF8

Compress-Archive -LiteralPath $PackageDirectory -DestinationPath $ZipPath -CompressionLevel Optimal

$StagedVendorSha = (Get-FileHash -LiteralPath (Join-Path $VendorDirectory.FullName 'ViKey.dll') -Algorithm SHA256).Hash
$StagedFormatSha = (Get-FileHash -LiteralPath (Join-Path $PackageDirectory 'VeraCryptFormat.dll') -Algorithm SHA256).Hash
$StagedVeraCryptInstallerSha = (Get-FileHash -LiteralPath (Join-Path $PrerequisitesDirectory.FullName 'VeraCrypt Setup 1.26.29.exe') -Algorithm SHA256).Hash
$StagedWebViewInstallerSha = (Get-FileHash -LiteralPath (Join-Path $PrerequisitesDirectory.FullName 'MicrosoftEdgeWebView2RuntimeInstallerX64.exe') -Algorithm SHA256).Hash
if ($StagedVendorSha -ne $VendorSha256 -or
    $StagedFormatSha -ne $FormatSha256 -or
    $StagedVeraCryptInstallerSha -ne $VeraCryptInstallerSha256 -or
    $StagedWebViewInstallerSha -ne $WebViewInstallerSha256) {
    throw 'A staged third-party runtime changed during packaging'
}

[PSCustomObject]@{
    PackageDirectory = $PackageDirectory
    ZipPath = $ZipPath
    ZipSha256 = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash
    SourceRevision = $GitRevision
    WorkingTreeState = $WorkingTreeState
}
