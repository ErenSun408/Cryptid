#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $PackageDirectory,
    [Parameter(Mandatory = $true)][string] $SetupHelper,
    [Parameter(Mandatory = $true)][string] $MakeNsis,
    [string] $OutputDirectory = (Join-Path $PSScriptRoot '..\out\installers')
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$package = (Get-Item -LiteralPath $PackageDirectory).FullName.TrimEnd('\')
$helper = (Get-Item -LiteralPath $SetupHelper).FullName
$compiler = (Get-Item -LiteralPath $MakeNsis).FullName
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$bundleName = 'installer-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss', [Globalization.CultureInfo]::InvariantCulture)
$output = Join-Path $outputRoot $bundleName

# Refuse inputs that could inject NSIS script or preprocessor syntax. The
# generated file list is independently checked against the package manifest.
foreach ($path in @($package, $helper, $compiler, $output)) {
    if ($path -match '["$\r\n;!]') { throw 'Unsupported character in build path' }
}
$manifest = Get-Content -LiteralPath "$package.sha256sums.txt"
$expected = @{}
foreach ($line in $manifest) {
    if ($line -notmatch '^([A-Fa-f0-9]{64})  (.+)$') { throw 'Invalid package manifest' }
    $relative = $Matches[2].Replace('/', '\')
    if ($relative -match '(^|\\)\.\.?($|\\)|[:"$\r\n;!*?]' -or $relative.StartsWith('\')) {
        throw 'Unsafe manifest path'
    }
    if ($expected.ContainsKey($relative)) { throw 'Duplicate manifest path' }
    $expected[$relative] = $Matches[1]
}
$files = @(Get-ChildItem -LiteralPath $package -Recurse -Force -File)
if ($files.Count -ne $expected.Count) { throw 'Package contents differ from manifest' }
foreach ($item in @(Get-Item -LiteralPath $package) + @(Get-ChildItem -LiteralPath $package -Recurse -Force)) {
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse package path rejected' }
}
foreach ($file in $files) {
    $relative = $file.FullName.Substring($package.Length + 1)
    if (!$expected.ContainsKey($relative) -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne $expected[$relative]) {
        throw "Unverified package file: $relative"
    }
}
foreach ($name in @('USB Vault.exe', 'usb_vault_vikey_helper.exe',
                   'usb_vault_volume_creator.exe', 'VeraCryptFormat.dll', 'card_session.html', 'card_session.js',
                   'vendor\ViKey.dll', 'prerequisites\veracrypt-x64.sys',
                   'prerequisites\MicrosoftEdgeWebView2RuntimeInstallerX64.exe', 'licenses\THIRD-PARTY-NOTICES.txt')) {
    if (!$expected.ContainsKey($name)) { throw "Required package file missing: $name" }
}
if ((Split-Path -Leaf $helper) -cne 'usb_vault_setup_helper.exe') { throw 'Unexpected helper name' }
if ($expected.ContainsKey('prerequisites\VeraCrypt_Setup_x64_1.26.29.msi')) {
    throw 'Legacy VeraCrypt MSI must not be included in the driver-only installer'
}
& (Join-Path $PSScriptRoot 'audit-veracrypt-driver.ps1') -DriverPath (Join-Path $package 'prerequisites\veracrypt-x64.sys') | Out-Null
$version = (& $compiler /VERSION | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $version -ne 'v3.12') { throw 'NSIS 3.12 is required' }
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
if (Test-Path -LiteralPath $output) { throw 'Installer bundle timestamp already exists; retry later or choose a new output directory' }
New-Item -ItemType Directory -Path $output | Out-Null
$target = Join-Path $output 'USB-Vault-Setup.exe'
# Bind recovery to both payload and implementation, including the generated
# uninstaller protocol. A changed package must never interpret an old journal.
$identityInputs = @((Get-FileHash -LiteralPath "$package.sha256sums.txt").Hash, (Get-FileHash -LiteralPath $helper).Hash)
$identityInputs += @(Get-ChildItem -LiteralPath (Join-Path $root 'packaging\windows') -File | Where-Object Extension -in @('.nsh','.nsi') | Sort-Object Name | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName).Hash })
$identityInputs += (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'installer-file-plan.ps1')).Hash
$sha = [Security.Cryptography.SHA256]::Create()
try { $packageIdentity = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($identityInputs -join "`n")))).Replace('-','') } finally { $sha.Dispose() }
. (Join-Path $PSScriptRoot 'installer-file-plan.ps1')
Write-InstallerFilePlan -Package $package -Files $files -Output $output -Identity $packageIdentity
$packagePlan = Join-Path $output 'package-plan.nsh'
& $compiler /NOCONFIG /INPUTCHARSET UTF8 /V2 "/DPayloadDirectory=$package" "/DSetupHelper=$helper" "/DOutputFile=$target" "/DPackagePlan=$packagePlan" (Join-Path $root 'packaging\windows\usb-vault.nsi')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed' }
$identity = @(
    "Compiler: $version",
    "Compiler SHA256: $((Get-FileHash -LiteralPath $compiler).Hash)",
    "Setup helper SHA256: $((Get-FileHash -LiteralPath $helper).Hash)",
    "Package manifest SHA256: $((Get-FileHash -LiteralPath "$package.sha256sums.txt").Hash)",
    "Installer SHA256: $((Get-FileHash -LiteralPath $target).Hash)"
)
Set-Content -LiteralPath "$target.build.txt" -Value $identity -Encoding UTF8
Get-Item -LiteralPath $target | Select-Object FullName, Length
