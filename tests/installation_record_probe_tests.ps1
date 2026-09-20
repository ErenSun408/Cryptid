#requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $MakeNsis)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$temporary = Join-Path $root ('out\installation-record-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$file = Join-Path $temporary 'ordinary-file'
Set-Content -LiteralPath $file -Value 'test'
$cases = @(
    @{ Name = 'existing-directory'; Path = $temporary; Expected = 10 },
    @{ Name = 'missing-directory'; Path = (Join-Path $temporary 'missing'); Expected = 0 },
    @{ Name = 'missing-parent'; Path = (Join-Path $temporary 'missing\child'); Expected = 0 },
    @{ Name = 'file-at-install-location'; Path = $file; Expected = 10 },
    @{ Name = 'invalid-path'; Path = 'C:\bad<name'; Expected = 20 }
)
foreach ($case in $cases) {
    $output = Join-Path $temporary ($case.Name + '.exe')
    & $MakeNsis /NOCONFIG /INPUTCHARSET UTF8 /V2 "/DTestOutput=$output" "/DTestPath=$($case.Path)" (Join-Path $PSScriptRoot 'installation_record_probe.nsi')
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($case.Name)" }
    $process = Start-Process -FilePath $output -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne $case.Expected) { throw "$($case.Name): expected $($case.Expected), got $($process.ExitCode)" }
    Write-Output "PASS $($case.Name)"
}
