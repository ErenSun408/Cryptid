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
$output = [IO.Path]::GetFullPath($OutputDirectory)

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
foreach ($name in @('usb_vault_vikey_p2_development_app.exe', 'usb_vault_vikey_helper.exe',
                   'usb_vault_volume_creator.exe', 'VeraCryptFormat.dll', 'card_session.html', 'card_session.js',
                   'vendor\ViKey.dll', 'prerequisites\VeraCrypt_Setup_x64_1.26.29.msi',
                   'prerequisites\MicrosoftEdgeWebView2RuntimeInstallerX64.exe', 'licenses\THIRD-PARTY-NOTICES.txt')) {
    if (!$expected.ContainsKey($name)) { throw "Required package file missing: $name" }
}
if ((Split-Path -Leaf $helper) -cne 'usb_vault_setup_helper.exe') { throw 'Unexpected helper name' }
$version = (& $compiler /VERSION | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $version -ne 'v3.12') { throw 'NSIS 3.12 is required' }
New-Item -ItemType Directory -Force -Path $output | Out-Null
$target = Join-Path $output 'USB-Vault-Setup.exe'
if (Test-Path -LiteralPath $target) { throw 'Installer already exists; choose a new output directory' }
$install = New-Object 'Collections.Generic.List[string]'
$uninstall = New-Object 'Collections.Generic.List[string]'
$directories = New-Object 'Collections.Generic.HashSet[string]'
foreach ($file in $files | Sort-Object FullName) {
    $relative = $file.FullName.Substring($package.Length + 1)
    if ($relative -eq 'usb_vault_vikey_development_credentials_config.exe') { continue }
    $parent = Split-Path -Parent $relative
    $destination = if ($parent) { '$INSTDIR\' + $parent } else { '$INSTDIR' }
    $install.Add('SetOutPath "' + $destination + '"')
    $install.Add('File "' + $file.FullName + '"')
    $uninstall.Add('Delete "$INSTDIR\' + $relative + '"')
    while ($parent) {
        $null = $directories.Add($parent)
        $parent = Split-Path -Parent $parent
    }
}
foreach ($dir in $directories | Sort-Object Length -Descending) { $uninstall.Add('RMDir "$INSTDIR\' + $dir + '"') }
$installFile = Join-Path $output 'install-files.nsh'
$uninstallFile = Join-Path $output 'uninstall-files.nsh'
Set-Content -LiteralPath $installFile -Value $install -Encoding UTF8
Set-Content -LiteralPath $uninstallFile -Value $uninstall -Encoding UTF8
& $compiler /NOCONFIG /INPUTCHARSET UTF8 /V2 "/DPayloadDirectory=$package" "/DSetupHelper=$helper" "/DOutputFile=$target" "/DInstallFiles=$installFile" "/DUninstallFiles=$uninstallFile" (Join-Path $root 'packaging\windows\usb-vault.nsi')
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
