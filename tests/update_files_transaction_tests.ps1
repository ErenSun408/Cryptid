#requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $MakeNsis)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path -Parent $PSScriptRoot
$temporary = Join-Path $root ('out\update-file-tests-' + [Guid]::NewGuid().ToString('N'))
function Assert-File([string] $Path, [string] $Expected) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or
        [IO.File]::ReadAllText($Path) -cne $Expected) { throw "Unexpected file contents: $Path" }
}
foreach ($case in @('replace', 'add', 'missing-stage', 'source-directory', 'target-directory', 'backup-collision', 'locked-old', 'locked-stage', 'rollback-add', 'rollback-blocked', 'cleanup-blocked')) {
    $fixture = Join-Path $temporary $case
    foreach ($directory in @('app', 'stage', 'backup')) {
        New-Item -ItemType Directory -Path (Join-Path $fixture $directory) | Out-Null
    }
    [IO.File]::WriteAllText("$fixture\app\user-container", 'user data stays')
    [IO.File]::WriteAllText("$fixture\app\a", 'old a')
    [IO.File]::WriteAllText("$fixture\app\b", 'old b')
    [IO.File]::WriteAllText("$fixture\stage\a", 'new a')
    [IO.File]::WriteAllText("$fixture\stage\b", 'new b')
    $defines = @()
    $expected = 10
    switch ($case) {
        'replace' { $expected = 0 }
        'add' { Remove-Item -LiteralPath "$fixture\app\a"; $expected = 0 }
        'missing-stage' { Remove-Item -LiteralPath "$fixture\stage\b" }
        'source-directory' { Remove-Item -LiteralPath "$fixture\stage\b"; New-Item -ItemType Directory -Path "$fixture\stage\b" | Out-Null }
        'target-directory' { Remove-Item -LiteralPath "$fixture\app\b"; New-Item -ItemType Directory -Path "$fixture\app\b" | Out-Null }
        'backup-collision' { [IO.File]::WriteAllText("$fixture\backup\b", 'existing backup') }
        'rollback-add' { Remove-Item -LiteralPath "$fixture\app\a"; $defines += '/DForceRollback' }
        'rollback-blocked' { $defines += '/DForceRollback'; $defines += '/DBlockRollback'; $expected = 20 }
        'cleanup-blocked' { $defines += '/DBlockCleanup'; $expected = 30 }
    }
    $output = Join-Path $fixture 'test.exe'
    & $MakeNsis /NOCONFIG /INPUTCHARSET UTF8 /V2 "/DTestOutput=$output" "/DTestRoot=$fixture" @defines (Join-Path $PSScriptRoot 'update_files_transaction.nsi')
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $case" }
    $lock = $null
    try {
        if ($case -eq 'locked-old') {
            $lock = [IO.File]::Open("$fixture\app\b", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
        }
        if ($case -eq 'locked-stage') {
            $lock = [IO.File]::Open("$fixture\stage\b", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
        }
        $process = Start-Process -FilePath $output -WindowStyle Hidden -PassThru -Wait
        if ($process.ExitCode -ne $expected) { throw "${case}: expected $expected, got $($process.ExitCode)" }
    } finally { if ($null -ne $lock) { $lock.Dispose() } }
    Assert-File "$fixture\app\user-container" 'user data stays'
    if ($expected -eq 0 -or $expected -eq 30) {
        Assert-File "$fixture\app\a" 'new a'
        Assert-File "$fixture\app\b" 'new b'
    } else {
        if ($case -eq 'rollback-add') {
            if (Test-Path -LiteralPath "$fixture\app\a") { throw 'New file survived rollback' }
        } elseif ($case -eq 'rollback-blocked') {
            Assert-File "$fixture\app\a" 'new a'
            Assert-File "$fixture\backup\a" 'old a'
        } else { Assert-File "$fixture\app\a" 'old a' }
        if ($case -eq 'target-directory') {
            if (!(Test-Path -LiteralPath "$fixture\app\b" -PathType Container)) { throw 'Unrelated directory removed' }
        } else { Assert-File "$fixture\app\b" 'old b' }
    }
    if ($case -eq 'cleanup-blocked') { Assert-File "$fixture\backup\a" 'old a' }
    elseif ($case -eq 'backup-collision') { Assert-File "$fixture\backup\b" 'existing backup' }
    elseif ($case -ne 'rollback-blocked' -and @(Get-ChildItem -LiteralPath "$fixture\backup" -Force).Count -ne 0) { throw 'Unexpected backup left behind' }
    Write-Output "PASS $case"
}
