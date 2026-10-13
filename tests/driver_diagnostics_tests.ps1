#requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string] $MakeNsis,
    [Parameter(Mandatory = $true)][string] $SetupHelper,
    [Parameter(Mandatory = $true)][string] $ConfigurationTests
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$helper = (Get-Item -LiteralPath $SetupHelper).FullName
$compiler = (Get-Item -LiteralPath $MakeNsis).FullName
$suite = Join-Path (Split-Path -Parent $PSScriptRoot) ('out\driver-diagnostics-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $suite | Out-Null
function Assert-Exit([string] $Arguments, [int] $Expected) {
    $process = Start-Process -FilePath $helper -ArgumentList $Arguments -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne $Expected) { throw "Unexpected helper exit $($process.ExitCode): $Arguments" }
}
Assert-Exit '--probe-driver-diagnostics' 0
foreach ($argument in @('--probe-driver-diagnostics unexpected', '--install-driver-elevated --diagnostic-pipe',
    '--install-driver-elevated --diagnostic-pipe ../../unexpected',
    '--install-driver-elevated --diagnostic-pipe 0123456789abcdef0123456789abcdeG',
    '--install-driver-elevated --diagnostic-pipe 0123456789abcdef0123456789abcdef unexpected',
    '--probe-veracrypt --diagnostic-pipe 0123456789abcdef0123456789abcdef')) {
    Assert-Exit $argument 64
}
Write-Output 'PASS helper diagnostic capability and strict argument validation (no driver installation)'
$probe = Start-Process -FilePath $helper -ArgumentList '--probe-veracrypt' -WindowStyle Hidden -PassThru -Wait
if ($probe.ExitCode -eq 0) {
    Assert-Exit '--veracrypt' 0
    Write-Output 'PASS existing ready driver is reused without installation or elevation'
} else {
    Write-Output 'SKIP ready-driver reuse: driver is not ready; this suite never installs or repairs it'
}
& $ConfigurationTests --helper-diagnostics $helper
if ($LASTEXITCODE -ne 0) { throw 'Real helper diagnostic pipe test failed' }
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
$definitions = @("/DOutputFile=$suite\diagnostics.exe", "/DSetupHelper=$helper")
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $definitions += '/DCheckUnelevatedHelper'
} else {
    Write-Output 'SKIP real helper token rejection: current process is elevated; no system driver operation permitted in this suite'
}
& $compiler /NOCONFIG /INPUTCHARSET UTF8 /V2 @definitions (Join-Path $PSScriptRoot 'driver_diagnostics.nsi')
if ($LASTEXITCODE -ne 0) { throw 'Diagnostic NSIS fixture compilation failed' }
$process = Start-Process -FilePath (Join-Path $suite 'diagnostics.exe') -WindowStyle Hidden -PassThru -Wait
if ($process.ExitCode -ne 0) { throw "Diagnostic NSIS fixture failed: $($process.ExitCode)" }
Write-Output 'PASS shared NSIS diagnostic formatting, full hexadecimal errors, fallback and register preservation'
if ($definitions -contains '/DCheckUnelevatedHelper') {
    Write-Output 'PASS real helper stdout through nsExec displays the exact elevation rejection and Win32 error'
}
Write-Output "Evidence: $suite"
