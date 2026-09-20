#requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $MakeNsis)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '..\tools\installer-file-plan.ps1')
$suite = Join-Path (Split-Path -Parent $PSScriptRoot) ('out\installer-engine-tests-' + [Guid]::NewGuid().ToString('N'))
$keys = [Collections.Generic.List[string]]::new()
# Set only the test directory DACL. Set-Acl may also request audit privileges
# that a normal user does not have; auditing is outside this test's scope.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.ComponentModel;
public static class InstallerTestAcl {
  [DllImport("advapi32.dll", SetLastError=true)]
  static extern bool GetSecurityDescriptorDacl(IntPtr sd, out bool present, out IntPtr dacl, out bool defaulted);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode)]
  static extern uint SetNamedSecurityInfo(string name, int type, uint info, IntPtr owner, IntPtr group, IntPtr dacl, IntPtr sacl);
  public static void Apply(string path, byte[] descriptor, bool protect) {
    var pinned = GCHandle.Alloc(descriptor, GCHandleType.Pinned);
    try {
      bool present, defaulted; IntPtr dacl;
      if (!GetSecurityDescriptorDacl(pinned.AddrOfPinnedObject(), out present, out dacl, out defaulted) || !present)
        throw new Win32Exception(Marshal.GetLastWin32Error());
      uint result = SetNamedSecurityInfo(path, 1, 4u | (protect ? 0x80000000u : 0x20000000u), IntPtr.Zero, IntPtr.Zero, dacl, IntPtr.Zero);
      if (result != 0) throw new Win32Exception((int)result);
    } finally { pinned.Free(); }
  }
}
'@
function New-Fixture([string] $Name) {
    $fixture = Join-Path $suite $Name
    $payload = Join-Path $fixture 'payload'
    New-Item -ItemType Directory -Path "$payload\nested" | Out-Null
    [IO.File]::WriteAllText("$payload\USB Vault.exe", 'new main')
    [IO.File]::WriteAllText("$payload\nested\asset", 'new asset')
    $id = [Guid]::NewGuid().ToString('N')
    $key = 'HKCU:\Software\USBVaultInstallerTests\' + $id
    $keys.Add($key)
    return @{ Root = $fixture; Payload = $payload; Id = $id; Key = $key }
}
function Compile-Fixture($Fixture, [string] $StopAt = 'never', [string] $Identity = ('A' * 64)) {
    $output = Join-Path $Fixture.Root ($StopAt + '-' + $Identity.Substring(0,1))
    New-Item -ItemType Directory -Force -Path $output | Out-Null
    Write-InstallerFilePlan -Package $Fixture.Payload -Files @(Get-ChildItem -LiteralPath $Fixture.Payload -Recurse -File) -Output $output -Identity $Identity
    $executable = Join-Path $output 'test.exe'
    & $MakeNsis /NOCONFIG /INPUTCHARSET UTF8 /V2 "/DTestOutput=$executable" "/DTestRoot=$($Fixture.Root)" "/DTestId=$($Fixture.Id)" "/DPackagePlan=$output\package-plan.nsh" "/DStopAt=$StopAt" (Join-Path $PSScriptRoot 'installer_engine.nsi') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Compile failed' }
    return $executable
}
function Run-Fixture([string] $Executable, [int] $Expected = 0, [string] $Arguments = '/S') {
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne $Expected) { throw "Expected $Expected, got $($process.ExitCode): $Executable" }
}
function Assert-Text([string] $Path, [string] $Expected) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or [IO.File]::ReadAllText($Path) -cne $Expected) { throw "Unexpected contents: $Path" }
}
function Assert-Installed($Fixture) {
    Assert-Text "$($Fixture.Root)\app\USB Vault.exe" 'new main'
    Assert-Text "$($Fixture.Root)\app\nested\asset" 'new asset'
    if (Test-Path -LiteralPath "$($Fixture.Root)\app\.usb-vault-update") { throw 'Transaction not cleaned' }
    if ((Get-ItemProperty -LiteralPath $Fixture.Key).InstallLocation -ne "$($Fixture.Root)\app") { throw 'Registration mismatch' }
    $shell = New-Object -ComObject WScript.Shell
    foreach ($name in @('USB Vault', '卸载 USB Vault')) {
        $shortcut = $shell.CreateShortcut("$($Fixture.Root)\menu\$name.lnk")
        if ($shortcut.WorkingDirectory -ne "$($Fixture.Root)\app") { throw 'Shortcut points to temporary working directory' }
        if ($shortcut.TargetPath -ne "$($Fixture.Root)\app\$name.exe" -or $shortcut.Arguments -ne '') { throw 'Shortcut target mismatch' }
    }
}
try {
    $fixture = New-Fixture 'install-update-repair-uninstall'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    Assert-Installed $fixture
    [IO.File]::WriteAllText("$($fixture.Root)\app\container.hc", 'user bytes')
    [IO.File]::WriteAllText("$($fixture.Root)\app\USB Vault.exe", 'old main')
    Run-Fixture $exe
    Assert-Installed $fixture
    Remove-Item -LiteralPath "$($fixture.Root)\app\USB Vault.exe"
    Run-Fixture $exe
    Assert-Installed $fixture
    Assert-Text "$($fixture.Root)\app\container.hc" 'user bytes'
    Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe"
    if (Test-Path -LiteralPath $fixture.Key) { throw 'Uninstall registration survived' }
    if (Test-Path -LiteralPath "$($fixture.Root)\app\USB Vault.exe") { throw 'Uninstall left program' }
    Assert-Text "$($fixture.Root)\app\container.hc" 'user bytes'
    Write-Output 'PASS install, update, missing-program repair, uninstall and data retention'

    foreach ($stop in @('intent-flushed','old-moved','new-moved','files-committed')) {
        $fixture = New-Fixture $stop
        $exe = Compile-Fixture $fixture
        Run-Fixture $exe
        [IO.File]::WriteAllText("$($fixture.Root)\app\container.hc", 'user bytes')
        [IO.File]::WriteAllText("$($fixture.Root)\app\USB Vault.exe", 'old main')
        $crash = Compile-Fixture $fixture $stop
        Run-Fixture $crash 99
        $transaction = "$($fixture.Root)\app\.usb-vault-update"
        if (!(Get-Acl -LiteralPath $transaction).AreAccessRulesProtected) { throw 'Transaction ACL inherits access' }
        Run-Fixture $exe
        Assert-Installed $fixture
        Assert-Text "$($fixture.Root)\app\container.hc" 'user bytes'
        Write-Output "PASS restart recovery after $stop"
    }

    $fixture = New-Fixture 'registration-failure'
    [IO.File]::WriteAllText("$($fixture.Root)\menu", 'menu collision')
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe 10
    if (!(Test-Path -LiteralPath "$($fixture.Root)\app\.usb-vault-update\committed")) { throw 'Failed metadata lost commit marker' }
    Assert-Text "$($fixture.Root)\menu" 'menu collision'
    Remove-Item -LiteralPath "$($fixture.Root)\menu"
    Run-Fixture $exe
    Assert-Installed $fixture
    Write-Output 'PASS metadata failure and forward recovery'

    $fixture = New-Fixture 'locked-product'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    [IO.File]::WriteAllText("$($fixture.Root)\app\USB Vault.exe", 'old main')
    $lease = [IO.File]::Open("$($fixture.Root)\app\nested\asset", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    try { Run-Fixture $exe 10 } finally { $lease.Dispose() }
    Assert-Text "$($fixture.Root)\app\USB Vault.exe" 'old main'
    Run-Fixture $exe
    Assert-Installed $fixture
    Write-Output 'PASS occupied product refuses update before replacement'

    $fixture = New-Fixture 'foreign-package'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    $crash = Compile-Fixture $fixture 'old-moved'
    Run-Fixture $crash 99
    $foreign = Compile-Fixture $fixture 'never' ('B' * 64)
    Run-Fixture $foreign 10
    if (!(Test-Path -LiteralPath "$($fixture.Root)\app\.usb-vault-update\backup\F0000")) { throw 'Foreign package lost backup' }
    # The NSIS launcher returns zero after its temporary child exits. Assert
    # refusal by retained registration and backup, not the launcher's status.
    Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe"
    if (!(Test-Path -LiteralPath $fixture.Key)) { throw 'Pending update uninstall removed registration' }
    if (!(Test-Path -LiteralPath "$($fixture.Root)\app\.usb-vault-update\backup\F0000")) { throw 'Pending uninstall removed recovery backup' }
    [IO.File]::WriteAllText("$($fixture.Root)\app\.usb-vault-update\unknown", 'keep unknown')
    Run-Fixture $exe 10
    Assert-Text "$($fixture.Root)\app\.usb-vault-update\unknown" 'keep unknown'
    Remove-Item -LiteralPath "$($fixture.Root)\app\.usb-vault-update\unknown"
    Run-Fixture $exe
    Assert-Installed $fixture
    Write-Output 'PASS foreign package, pending uninstall and unknown entries rejected'

    $fixture = New-Fixture 'altered-acl'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    $crash = Compile-Fixture $fixture 'old-moved'
    Run-Fixture $crash 99
    $transaction = "$($fixture.Root)\app\.usb-vault-update"
    $protected = Get-Acl -LiteralPath $transaction
    [InstallerTestAcl]::Apply($transaction, $protected.GetSecurityDescriptorBinaryForm(), $false)
    Run-Fixture $exe 10
    if (!(Test-Path -LiteralPath "$transaction\backup\F0000")) { throw 'Altered ACL refusal lost backup' }
    Write-Output 'PASS altered ACL rejected with backup retained'

    $fixture = New-Fixture 'junction-parent'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    Rename-Item -LiteralPath "$($fixture.Root)\app\nested" -NewName 'original-nested'
    New-Item -ItemType Directory -Path "$($fixture.Root)\outside" | Out-Null
    [IO.File]::WriteAllText("$($fixture.Root)\outside\asset", 'outside stays')
    New-Item -ItemType Junction -Path "$($fixture.Root)\app\nested" -Target "$($fixture.Root)\outside" | Out-Null
    Run-Fixture $exe 10
    Assert-Text "$($fixture.Root)\outside\asset" 'outside stays'
    Write-Output 'PASS reparse parent cannot redirect replacement'

    $fixture = New-Fixture 'nonempty-unregistered'
    New-Item -ItemType Directory -Path "$($fixture.Root)\app" | Out-Null
    [IO.File]::WriteAllText("$($fixture.Root)\app\user", 'keep')
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe 10
    Assert-Text "$($fixture.Root)\app\user" 'keep'
    if (Test-Path -LiteralPath "$($fixture.Root)\app\.usb-vault-update") { throw 'Nonempty folder modified' }
    Write-Output 'PASS unregistered nonempty directory rejected'
} finally {
    foreach ($key in $keys) { if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key } }
}
