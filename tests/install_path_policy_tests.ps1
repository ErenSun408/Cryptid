#requires -Version 5.1
param([Parameter(Mandatory = $true)][string] $MakeNsis)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$temporary = Join-Path $root ('out\install-path-policy-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
$output = Join-Path $temporary 'policy.exe'
& $MakeNsis /NOCONFIG /INPUTCHARSET UTF8 /V2 "/DTestOutput=$output" "/DTestInput=$temporary\input.txt" (Join-Path $PSScriptRoot 'install_path_policy.nsi')
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
# Exit code = policy result + 10: 11 ASCII only, 10 non-ASCII.
$cases = @(
    @{ Name = 'default-ascii'; Path = 'C:\Users\alice\AppData\Local\Programs\USB Vault'; Expected = 11 },
    @{ Name = 'ascii-symbols'; Path = 'D:\Tools (x86)\USB-Vault_1.0 & more\#1'; Expected = 11 },
    @{ Name = 'chinese-folder'; Path = "D:\$([char]0x5B89)$([char]0x88C5)\USB Vault"; Expected = 10 },
    @{ Name = 'chinese-user'; Path = "C:\Users\$([char]0x5F20)$([char]0x4E09)\AppData\Local\Programs\USB Vault"; Expected = 10 },
    @{ Name = 'accented-latin'; Path = "D:\Caf$([char]0xE9)\USB Vault"; Expected = 10 },
    @{ Name = 'fullwidth-letter'; Path = "D:\$([char]0xFF35)SB Vault"; Expected = 10 },
    @{ Name = 'japanese-kana'; Path = "D:\$([char]0x30A2)\USB Vault"; Expected = 10 },
    @{ Name = 'emoji-surrogate'; Path = "D:\x$([char]0xD83D)$([char]0xDE00)\USB Vault"; Expected = 10 },
    @{ Name = 'nbsp'; Path = "D:\USB$([char]0xA0)Vault"; Expected = 10 }
)
foreach ($case in $cases) {
    [IO.File]::WriteAllText("$temporary\input.txt", $case.Path, [Text.Encoding]::Unicode)
    $process = Start-Process -FilePath $output -WindowStyle Hidden -PassThru -Wait
    if ($process.ExitCode -ne $case.Expected) { throw "$($case.Name): expected $($case.Expected), got $($process.ExitCode)" }
    Write-Output "PASS $($case.Name)"
}
Remove-Item -LiteralPath $temporary -Recurse -Force
