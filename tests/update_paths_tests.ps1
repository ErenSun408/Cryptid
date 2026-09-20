#requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $MakeNsis)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $root ('out\update-path-tests-' + [Guid]::NewGuid().ToString('N'))
$plain = Join-Path $fixture 'plain\child'
New-Item -ItemType Directory -Path $plain | Out-Null
$junction = Join-Path $fixture 'junction'
New-Item -ItemType Junction -Path $junction -Target (Join-Path $fixture 'plain') | Out-Null
$cases = @(
    @{ Name = 'plain'; Path = $plain; Code = 0; Defines = @() },
    @{ Name = 'junction-leaf'; Path = $junction; Code = 1; Defines = @() },
    @{ Name = 'junction-ancestor'; Path = "$junction\child"; Code = 1; Defines = @() },
    @{ Name = 'relative'; Path = 'out'; Code = 1; Defines = @() },
    @{ Name = 'drive-relative'; Path = 'D:out'; Code = 1; Defines = @() },
    @{ Name = 'drive-root'; Path = 'D:\'; Code = 1; Defines = @() },
    @{ Name = 'unc'; Path = '\\server\share\folder'; Code = 1; Defines = @() },
    @{ Name = 'device-namespace'; Path = '\\?\D:\folder'; Code = 1; Defines = @() },
    @{ Name = 'alternate-stream'; Path = "${plain}:stream"; Code = 1; Defines = @() },
    @{ Name = 'slash-alias'; Path = $plain.Replace('\','/'); Code = 1; Defines = @() },
    @{ Name = 'dot-segment'; Path = "$plain\..\child"; Code = 1; Defines = @() },
    @{ Name = 'trailing-space'; Path = "$plain "; Code = 1; Defines = @() },
    @{ Name = 'trailing-dot'; Path = "$plain."; Code = 1; Defines = @() },
    @{ Name = 'trailing-slash'; Path = "$plain\"; Code = 1; Defines = @() },
    @{ Name = 'missing'; Path = "$plain\missing"; Code = 1; Defines = @() },
    @{ Name = 'pinned-directory'; Path = $plain; Code = 0; Defines = @('/DTestRename') },
    @{ Name = 'exclusive-lock'; Path = $plain; Code = 0; Defines = @('/DTestLock') }
)
foreach ($case in $cases) {
    $output = Join-Path $fixture ($case.Name + '.exe')
    $defines = $case.Defines
    & $MakeNsis /NOCONFIG /INPUTCHARSET UTF8 /V2 "/DTestOutput=$output" "/DTestPath=$($case.Path)" @defines (Join-Path $PSScriptRoot 'update_paths.nsi')
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $($case.Name)" }
    $process = Start-Process -FilePath $output -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne $case.Code) { throw "$($case.Name): expected $($case.Code), got $($process.ExitCode)" }
    if (Test-Path -LiteralPath "$plain\update.lock") { throw 'Lock not removed after handle release' }
    Write-Output "PASS $($case.Name)"
}
$collision = Join-Path $fixture 'lock-collision.exe'
[IO.File]::WriteAllText("$plain\update.lock", 'unrelated existing file')
& $MakeNsis /NOCONFIG /INPUTCHARSET UTF8 /V2 "/DTestOutput=$collision" "/DTestPath=$plain" /DTestLock (Join-Path $PSScriptRoot 'update_paths.nsi')
if ($LASTEXITCODE -ne 0) { throw 'Lock collision fixture compilation failed' }
$process = Start-Process -FilePath $collision -WindowStyle Hidden -PassThru -Wait
if ($process.ExitCode -ne 1 -or [IO.File]::ReadAllText("$plain\update.lock") -cne 'unrelated existing file') { throw 'Existing lock path not preserved' }
Write-Output 'PASS lock-collision'
# Handles must be released, including those accumulated on a failed guard.
Rename-Item -LiteralPath $plain -NewName 'released'
Write-Output 'PASS handles-released'
