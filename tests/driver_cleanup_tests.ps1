#requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $SetupHelper)
$ErrorActionPreference = 'Stop'
$helper = (Get-Item -LiteralPath $SetupHelper).FullName
function Assert-Exit([string] $Arguments, [int] $Expected) {
    $process = Start-Process -FilePath $helper -ArgumentList $Arguments -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne $Expected) { throw "Unexpected helper exit $($process.ExitCode): $Arguments" }
}
Assert-Exit '--probe-driver-cleanup' 0
foreach ($mode in @('--uninstall-veracrypt', '--uninstall-driver-elevated', '--probe-driver-cleanup')) {
    Assert-Exit "$mode unexpected" 64
}
Write-Output 'PASS read-only capability and strict argument rejection (no system uninstall)'
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Assert-Exit '--uninstall-driver-elevated' 12
    Write-Output 'PASS elevated cleanup rejects a non-elevated process before accessing driver state'
} else {
    Write-Output 'SKIP token rejection: current process is elevated; never invoke system removal in this suite'
}
