#requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $MakeNsis)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '..\tools\installer-file-plan.ps1')
# Windows PowerShell 5.1 decodes BOM-less scripts as ANSI, corrupting Chinese
# literals before NSIS even reads the generated include. Exercise that host.
$encodingRoot = Join-Path (Split-Path -Parent $PSScriptRoot) ('out\installer-encoding-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $encodingRoot | Out-Null
$planScript = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\tools\installer-file-plan.ps1')).Replace("'", "''")
$quotedEncodingRoot = $encodingRoot.Replace("'", "''")
$encodingCommand = ". '$planScript'; Write-InstallerFilePlan -Package '$quotedEncodingRoot' -Files @() -Output '$quotedEncodingRoot' -Identity 'ENCODINGPROBE'"
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command $encodingCommand
if ($LASTEXITCODE -ne 0 -or ![IO.File]::ReadAllText((Join-Path $encodingRoot 'package-plan.nsh')).Contains('卸载 USB Vault.exe')) { throw 'Windows PowerShell 5.1 corrupted the uninstaller filename' }
Write-Output 'PASS Windows PowerShell 5.1 preserves the Chinese file plan'
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
public static class InstallerTestCredentials {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  struct Credential {
    public uint Flags, Type;
    public string TargetName, Comment;
    public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
    public uint CredentialBlobSize;
    public IntPtr CredentialBlob;
    public uint Persist, AttributeCount;
    public IntPtr Attributes;
    public string TargetAlias, UserName;
  }
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern bool CredWrite(ref Credential credential, uint flags);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern bool CredRead(string target, uint type, uint flags, out IntPtr credential);
  [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern bool CredDelete(string target, uint type, uint flags);
  [DllImport("advapi32.dll")] static extern void CredFree(IntPtr credential);
  static void Guard(string target) {
    if (!target.StartsWith("usb-vault-installer-test-", StringComparison.Ordinal)) throw new ArgumentException("Non-test target");
  }
  public static void Write(string target) {
    Guard(target);
    var credential = new Credential { Type=1, TargetName=target, Persist=2, UserName="isolated-test" };
    if (!CredWrite(ref credential, 0)) throw new Win32Exception(Marshal.GetLastWin32Error());
  }
  public static bool Exists(string target) {
    Guard(target); IntPtr credential;
    if (CredRead(target, 1, 0, out credential)) { CredFree(credential); return true; }
    int error = Marshal.GetLastWin32Error();
    if (error != 1168) throw new Win32Exception(error);
    return false;
  }
  public static void Delete(string target) {
    Guard(target);
    if (!CredDelete(target, 1, 0) && Marshal.GetLastWin32Error() != 1168) throw new Win32Exception(Marshal.GetLastWin32Error());
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
    $uninstallCompletion = $null
    if ((Split-Path -Leaf $Executable) -eq '卸载 USB Vault.exe') {
        # NSIS normally re-executes a temporary copy; that parent reports 0
        # even when its child refuses uninstall. Run our own isolated copy with
        # _?= last so we wait for and inspect the actual uninstaller exit code.
        $installedDirectory = Split-Path -Parent $Executable
        $uninstallCompletion = Join-Path (Split-Path -Parent $installedDirectory) 'uninstall-completed.txt'
        if ($Expected -eq 10) { $Expected = 1603 }
        $runner = Join-Path (Split-Path -Parent $installedDirectory) ('uninstall-runner-' + [Guid]::NewGuid().ToString('N') + '.exe')
        Copy-Item -LiteralPath $Executable -Destination $runner
        $Executable = $runner
        $Arguments += ' _?=' + $installedDirectory
    }
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne $Expected) { throw "Expected $Expected, got $($process.ExitCode): $Executable" }
    if ($uninstallCompletion -and ((Test-Path -LiteralPath $uninstallCompletion) -ne ($Expected -eq 0))) {
        throw 'Failed uninstall reached its success continuation, or successful uninstall did not complete'
    }
}
function Assert-Text([string] $Path, [string] $Expected) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or [IO.File]::ReadAllText($Path) -cne $Expected) { throw "Unexpected contents: $Path" }
}
function Assert-Installed($Fixture) {
    Assert-Text "$($Fixture.Root)\app\USB Vault.exe" 'new main'
    if (!(Test-Path -LiteralPath "$($Fixture.Root)\app\卸载 USB Vault.exe")) { throw 'Unicode uninstaller filename was corrupted' }
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
function Configuration-Targets($Fixture) {
    foreach ($suffix in @('credentials', 'user-sn', 'api-key', 'user-pin', 'unrelated')) {
        'usb-vault-installer-test-' + $Fixture.Id + '-' + $suffix
    }
}
try {
    foreach ($configuration in @(0, 1)) {
            foreach ($driver in @(0, 1)) {
                $fixture = New-Fixture "cleanup-choice-$configuration-$driver"
                $exe = Compile-Fixture $fixture
                Run-Fixture $exe
                [IO.File]::WriteAllText("$($fixture.Root)\app\container.hc", 'container stay')
                [IO.File]::WriteAllText("$($fixture.Root)\app\self-stored.txt", 'personal stay')
                Assert-Installed $fixture
                Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" -Arguments "/S /DELETECONFIG=$configuration /DELETEDRIVER=$driver"
                $calls = if (Test-Path -LiteralPath "$($fixture.Root)\cleanup-calls.txt") { [IO.File]::ReadAllText("$($fixture.Root)\cleanup-calls.txt") } else { '' }
                if (!$calls.Contains('--cleanup-application-data') -or
                    $calls.Contains('--uninstall-webview') -or
                    $calls.Contains('--uninstall-veracrypt') -ne ($driver -eq 1)) { throw 'Cleanup choices are coupled' }
                if (Test-Path -LiteralPath $fixture.Key) { throw 'Successful cleanup left registration' }
                Assert-Text "$($fixture.Root)\app\container.hc" 'container stay'
                Assert-Text "$($fixture.Root)\app\self-stored.txt" 'personal stay'
            }
    }
    Write-Output 'PASS four cleanup choices preserve shared WebView2 and user files, with correct Unicode uninstaller filename'

    foreach ($status in @(11, 12, 20, 21, 22)) {
        $fixture = New-Fixture "driver-failure-$status"
        $exe = Compile-Fixture $fixture
        Run-Fixture $exe
        Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" 10 -Arguments "/S /DELETECONFIG=1 /DELETEDRIVER=1 /CLEANUPSTATUS=$status"
        Assert-Installed $fixture
        $calls = [IO.File]::ReadAllText("$($fixture.Root)\cleanup-calls.txt")
        if (!$calls.Contains('--uninstall-veracrypt') -or $calls.Contains('--cleanup-application-data') -or $calls.Contains('--uninstall-webview')) { throw 'Driver failure did not stop later cleanup' }
        Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" -Arguments '/S /DELETECONFIG=0 /DELETEDRIVER=0'
        if (Test-Path -LiteralPath $fixture.Key) { throw 'Opting out did not recover driver cleanup failure' }
    }
    Write-Output 'PASS blocked, unavailable, restart, cancelled and failed driver cleanup retains application and supports opting out'

    $fixture = New-Fixture 'profile-failure-retains-product'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" 10 -Arguments '/S /DELETECONFIG=1 /CLEANUPSTATUS=12'
    Assert-Installed $fixture
    $calls = [IO.File]::ReadAllText("$($fixture.Root)\cleanup-calls.txt")
    if (!$calls.Contains('--cleanup-application-data') -or $calls.Contains('--uninstall-webview')) { throw 'Failed profile cleanup still removed runtime' }
    Write-Output 'PASS profile cleanup failure retains the product and reports failure without success continuation'

    foreach ($delete in @(0, 1)) {
        $fixture = New-Fixture "configuration-choice-$delete"
        $exe = Compile-Fixture $fixture
        Run-Fixture $exe
        New-Item -ItemType Directory -Path "$($fixture.Root)\config" | Out-Null
        [IO.File]::WriteAllText("$($fixture.Root)\config\settings.txt", 'settings')
        [IO.File]::WriteAllText("$($fixture.Root)\config\container_index.bin", 'encrypted index')
        [IO.File]::WriteAllText("$($fixture.Root)\config\container.hc", 'container stays')
        [IO.File]::WriteAllText("$($fixture.Root)\app\user.txt", 'user file stays')
        foreach ($target in @(Configuration-Targets $fixture)) { [InstallerTestCredentials]::Write($target) }
        Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" -Arguments "/S /DELETECONFIG=$delete"
        foreach ($name in @('settings.txt', 'container_index.bin')) {
            if ((Test-Path -LiteralPath "$($fixture.Root)\config\$name") -ne ($delete -eq 0)) { throw 'Configuration choice ignored' }
        }
        foreach ($target in @(Configuration-Targets $fixture)) {
            $expected = $delete -eq 0 -or $target.EndsWith('-unrelated')
            if ([InstallerTestCredentials]::Exists($target) -ne $expected) { throw 'Credential scope/choice ignored' }
        }
        Assert-Text "$($fixture.Root)\config\container.hc" 'container stays'
        Assert-Text "$($fixture.Root)\app\user.txt" 'user file stays'
        if (Test-Path -LiteralPath $fixture.Key) { throw 'Configuration uninstall left registration' }
    }
    Write-Output 'PASS configuration choices and exact credential scope preserve containers and unrelated files'

    $fixture = New-Fixture 'configuration-absent'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" -Arguments '/S /DELETECONFIG=1'
    if (Test-Path -LiteralPath $fixture.Key) { throw 'Absent configuration blocked uninstall' }
    Write-Output 'PASS absent configuration and credentials are idempotent'

    $fixture = New-Fixture 'credentials-without-directory'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    foreach ($target in @(Configuration-Targets $fixture)) { [InstallerTestCredentials]::Write($target) }
    Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" -Arguments '/S /DELETECONFIG=1'
    foreach ($target in @(Configuration-Targets $fixture)) {
        if ([InstallerTestCredentials]::Exists($target) -ne $target.EndsWith('-unrelated')) { throw 'Credential cleanup depends on configuration directory' }
    }
    Write-Output 'PASS credentials removed even without a configuration directory'

    $fixture = New-Fixture 'configuration-file-is-directory'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    New-Item -ItemType Directory -Path "$($fixture.Root)\config\settings.txt" | Out-Null
    [IO.File]::WriteAllText("$($fixture.Root)\config\settings.txt\user", 'user stays')
    foreach ($target in @(Configuration-Targets $fixture)) { [InstallerTestCredentials]::Write($target) }
    Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" 10 -Arguments '/S /DELETECONFIG=1'
    Assert-Installed $fixture
    Assert-Text "$($fixture.Root)\config\settings.txt\user" 'user stays'
    foreach ($target in @(Configuration-Targets $fixture)) {
        if (![InstallerTestCredentials]::Exists($target)) { throw 'Unsafe file preflight deleted credentials' }
    }
    Write-Output 'PASS configuration file replaced by a directory rejects cleanup before credentials'

    $fixture = New-Fixture 'configuration-junction'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    New-Item -ItemType Directory -Path "$($fixture.Root)\outside" | Out-Null
    [IO.File]::WriteAllText("$($fixture.Root)\outside\settings.txt", 'outside stays')
    New-Item -ItemType Junction -Path "$($fixture.Root)\config" -Target "$($fixture.Root)\outside" | Out-Null
    Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" 10 -Arguments '/S /DELETECONFIG=1'
    Assert-Text "$($fixture.Root)\outside\settings.txt" 'outside stays'
    Assert-Installed $fixture
    Write-Output 'PASS redirected configuration rejected before product deletion'

    $fixture = New-Fixture 'configuration-locked'
    $exe = Compile-Fixture $fixture
    Run-Fixture $exe
    New-Item -ItemType Directory -Path "$($fixture.Root)\config" | Out-Null
    [IO.File]::WriteAllText("$($fixture.Root)\config\settings.txt", 'settings stay')
    $locked = [IO.File]::Open("$($fixture.Root)\config\settings.txt", 'Open', 'Read', 'Read')
    try { Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" 10 -Arguments '/S /DELETECONFIG=1' } finally { $locked.Dispose() }
    Assert-Text "$($fixture.Root)\config\settings.txt" 'settings stay'
    Assert-Installed $fixture
    Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" -Arguments '/S /DELETECONFIG=1'
    if (Test-Path -LiteralPath "$($fixture.Root)\config") { throw 'Empty configuration directory survived' }
    Write-Output 'PASS occupied configuration retains product and supports retry'

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
    Run-Fixture "$($fixture.Root)\app\卸载 USB Vault.exe" 10
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
    foreach ($key in $keys) {
        $id = Split-Path -Leaf $key
        foreach ($target in @(Configuration-Targets @{ Id = $id })) { [InstallerTestCredentials]::Delete($target) }
    }
    foreach ($key in $keys) { if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key } }
}
