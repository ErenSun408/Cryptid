#requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $SetupHelper)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$helper = (Get-Item -LiteralPath $SetupHelper).FullName
$suite = Join-Path (Split-Path -Parent $PSScriptRoot) ('out\webview-cleanup-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite | Out-Null
$probe = Start-Process -FilePath $helper -ArgumentList '--probe-uninstall-cleanup' -WindowStyle Hidden -PassThru -Wait
if ($probe.ExitCode -ne 0) { throw 'Cleanup capability probe failed' }
function Run-Cleanup([string] $Application, [int] $Expected = 0) {
    $process = Start-Process -FilePath $helper -ArgumentList ('--cleanup-webview-profile "' + $Application + '"') -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne $Expected) { throw "Expected $Expected, got $($process.ExitCode)" }
}
function New-Application([string] $Name) {
    $app = Join-Path $suite $Name
    New-Item -ItemType Directory -Path "$app\USB Vault.exe.WebView2\EBWebView\Default\Cache" | Out-Null
    [IO.File]::WriteAllText("$app\USB Vault.exe.WebView2\EBWebView\Default\Cache\entry", 'cache')
    [IO.File]::WriteAllText("$app\container.custom", 'container stays')
    [IO.File]::WriteAllText("$app\USB Vault.exe.WebView2\self-stored", 'user stays')
    return $app
}
function Assert-Text([string] $Path, [string] $Text) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or [IO.File]::ReadAllText($Path) -cne $Text) { throw "Unexpected file: $Path" }
}
$outside = Join-Path $suite 'outside'
New-Item -ItemType Directory -Path $outside | Out-Null
[IO.File]::WriteAllText("$outside\sentinel", 'outside stays')

$app = New-Application 'app with spaces'
Run-Cleanup $app
if (Test-Path -LiteralPath "$app\USB Vault.exe.WebView2\EBWebView") { throw 'Browser data remained' }
Assert-Text "$app\container.custom" 'container stays'
Assert-Text "$app\USB Vault.exe.WebView2\self-stored" 'user stays'
Run-Cleanup $app
Write-Output 'PASS real helper command line, spaced paths, retry and unknown sibling preservation'

$app = New-Application 'nested-junction'
New-Item -ItemType Junction -Path "$app\USB Vault.exe.WebView2\EBWebView\redirect" -Target $outside | Out-Null
Run-Cleanup $app 12
Assert-Text "$outside\sentinel" 'outside stays'
Assert-Text "$app\USB Vault.exe.WebView2\EBWebView\Default\Cache\entry" 'cache'
Write-Output 'PASS nested profile junction rejected before deleting cache'

$app = Join-Path $suite 'root-junction'
New-Item -ItemType Directory -Path $app | Out-Null
New-Item -ItemType Junction -Path "$app\USB Vault.exe.WebView2" -Target $outside | Out-Null
Run-Cleanup $app 12
Assert-Text "$outside\sentinel" 'outside stays'
Write-Output 'PASS profile root junction rejected'

$app = Join-Path $suite 'ancestor-junction'
New-Item -ItemType Junction -Path $app -Target $outside | Out-Null
Run-Cleanup $app 12
Assert-Text "$outside\sentinel" 'outside stays'
Write-Output 'PASS application ancestry junction rejected'

$app = New-Application 'occupied-profile'
$lockedPath = "$app\USB Vault.exe.WebView2\EBWebView\Default\Cache\entry"
$lock = [IO.File]::Open($lockedPath, 'Open', 'ReadWrite', 'Read')
try { Run-Cleanup $app 12 } finally { $lock.Dispose() }
Assert-Text $lockedPath 'cache'
Run-Cleanup $app
Assert-Text "$app\container.custom" 'container stays'
Write-Output 'PASS real helper refuses occupied profile and succeeds after release'

$app = New-Application 'arguments'
$process = Start-Process -FilePath $helper -ArgumentList ('--cleanup-webview-profile "' + $app + '" extra') -WindowStyle Hidden -PassThru -Wait
if ($process.ExitCode -ne 64) { throw 'Additional cleanup arguments accepted' }
Assert-Text "$app\USB Vault.exe.WebView2\EBWebView\Default\Cache\entry" 'cache'
Write-Output 'PASS extra helper arguments rejected without cleanup'
# Preserve isolated fixtures, including junctions, for review. Never run a
# recursive shell deletion across these deliberately redirected paths.
