#requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $MakeNsis)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$temporary = Join-Path $root ('out\update-recovery-tests-' + [Guid]::NewGuid().ToString('N'))
function Compile-Test([string] $Fixture, [string] $StopAt) {
    $output = Join-Path $Fixture ($StopAt + '.exe')
    & $MakeNsis /NOCONFIG /INPUTCHARSET UTF8 /V2 "/DTestOutput=$output" "/DTestRoot=$Fixture" "/DStopAt=$StopAt" (Join-Path $PSScriptRoot 'update_recovery.nsi') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $StopAt" }
    return $output
}
function Run-Test([string] $Executable, [string] $Mode, [int] $Expected) {
    $process = Start-Process -FilePath $Executable -ArgumentList $Mode -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne $Expected) { throw "${Executable} ${Mode}: expected $Expected, got $($process.ExitCode)" }
}
function Assert-Text([string] $Path, [string] $Text) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or [IO.File]::ReadAllText($Path) -cne $Text) { throw "Unexpected contents: $Path" }
}
foreach ($case in @('intent-flushed', 'old-moved', 'new-moved', 'committed', 'originally-absent', 'new-restaged', 'old-restored', 'corrupt-intent', 'corrupt-commit', 'wrong-intent-value', 'wrong-commit-value', 'stage-junction', 'missing-backup', 'ambiguous', 'locked-recovery', 'orphan-backup', 'intent-collision')) {
    $fixture = Join-Path $temporary $case
    foreach ($directory in @('app', 'stage', 'backup')) { New-Item -ItemType Directory -Path "$fixture\$directory" | Out-Null }
    [IO.File]::WriteAllText("$fixture\app\user-container", 'user data stays')
    [IO.File]::WriteAllText("$fixture\app\a", 'old a')
    [IO.File]::WriteAllText("$fixture\stage\a", 'new a')
    if ($case -eq 'originally-absent') { Remove-Item -LiteralPath "$fixture\app\a" }
    $stop = if ($case -in @('intent-flushed', 'old-moved', 'new-moved', 'committed')) { $case } else { 'new-moved' }
    $writer = Compile-Test $fixture $stop
    $reader = Compile-Test $fixture 'never'
    if ($case -eq 'intent-collision') {
        [IO.File]::WriteAllText("$fixture\backup\a.intent", 'existing recovery information')
        Run-Test $writer replace 10
        Assert-Text "$fixture\app\a" 'old a'
        Assert-Text "$fixture\stage\a" 'new a'
        Assert-Text "$fixture\backup\a.intent" 'existing recovery information'
    } elseif ($case -eq 'orphan-backup') {
        [IO.File]::WriteAllText("$fixture\backup\a", 'orphan backup')
        Run-Test $reader recover 20
        Assert-Text "$fixture\app\a" 'old a'
        Assert-Text "$fixture\backup\a" 'orphan backup'
    } else {
        Run-Test $writer replace 99
        if (Test-Path -LiteralPath "$fixture\backup\update.lock") { throw 'Lock survived process termination' }
        if ($case -in @('new-restaged', 'old-restored')) {
            $interruptedReader = Compile-Test $fixture $case
            Run-Test $interruptedReader recover 99
        }
        switch ($case) {
            'corrupt-intent' { [IO.File]::WriteAllText("$fixture\backup\a.intent", 'broken') }
            'corrupt-commit' { [IO.File]::WriteAllText("$fixture\backup\committed", 'broken') }
            'wrong-intent-value' { [IO.File]::WriteAllBytes("$fixture\backup\a.intent", [BitConverter]::GetBytes([int]827741442)) }
            'wrong-commit-value' { [IO.File]::WriteAllBytes("$fixture\backup\committed", [BitConverter]::GetBytes([int]827741441)) }
            'stage-junction' { New-Item -ItemType Junction -Path "$fixture\stage\a" -Target "$fixture\app" | Out-Null }
            'missing-backup' { Move-Item -LiteralPath "$fixture\backup\a" -Destination "$fixture\backup\saved-a" }
            'ambiguous' { [IO.File]::WriteAllText("$fixture\stage\a", 'unrelated staged data') }
        }
        if ($case -in @('corrupt-intent', 'corrupt-commit', 'wrong-intent-value', 'wrong-commit-value', 'stage-junction', 'missing-backup', 'ambiguous')) {
            Run-Test $reader recover 20
            Assert-Text "$fixture\app\a" 'new a'
            if ($case -eq 'missing-backup') { Assert-Text "$fixture\backup\saved-a" 'old a' }
            else { Assert-Text "$fixture\backup\a" 'old a' }
        } else {
            if ($case -eq 'locked-recovery') {
                $lock = [IO.File]::Open("$fixture\app\a", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
                try { Run-Test $reader recover 20 } finally { $lock.Dispose() }
                Assert-Text "$fixture\backup\a" 'old a'
            }
            Run-Test $reader recover 0
            Run-Test $reader recover 0 # Repeated recovery is idempotent.
            if ($case -eq 'committed') { Assert-Text "$fixture\app\a" 'new a' }
            elseif ($case -eq 'originally-absent') {
                if (Test-Path -LiteralPath "$fixture\app\a") { throw 'New file survived rollback' }
                Assert-Text "$fixture\stage\a" 'new a'
            } else {
                Assert-Text "$fixture\app\a" 'old a'
                Assert-Text "$fixture\stage\a" 'new a'
            }
            if (Test-Path -LiteralPath "$fixture\backup\a") { throw 'Recovered backup not consumed' }
            if (Test-Path -LiteralPath "$fixture\backup\a.intent") { throw 'Recovered intent not retired' }
        }
    }
    Assert-Text "$fixture\app\user-container" 'user data stays'
    Write-Output "PASS $case"
}
