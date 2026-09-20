# Compile-time NSIS file plan. The caller verifies all source hashes first.
function Write-InstallerFilePlan {
    param([string] $Package, [IO.FileInfo[]] $Files, [string] $Output, [string] $Identity)
    $plan = [Collections.Generic.List[string]]::new()
    $items = [Collections.Generic.List[object]]::new()
    $directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $index = 0
    $orderedPaths = [string[]]@($Files | ForEach-Object FullName)
    [Array]::Sort($orderedPaths, [StringComparer]::Ordinal)
    foreach ($sourcePath in $orderedPaths) {
        $file = Get-Item -LiteralPath $sourcePath
        $relative = $file.FullName.Substring($Package.Length + 1)
        if ($relative -match '(^|\\)\.|[:"$\r\n;!*?<>|]' -or $relative -match '(^|\\)[^\\]*[. ]($|\\)' -or $relative -eq '卸载 USB Vault.exe') { throw 'Unsafe or reserved installation entry' }
        $items.Add(@{ Id = ('F{0:D4}' -f $index); Relative = $relative; Source = $file.FullName })
        $index++
        $parent = Split-Path -Parent $relative
        while ($parent) { $null = $directories.Add($parent); $parent = Split-Path -Parent $parent }
    }
    # The generated uninstaller participates in the same file transaction.
    $items.Add(@{ Id = 'Uninstaller'; Relative = '卸载 USB Vault.exe'; Source = $null })
    $plan.Add('!define PackageIdentity "' + $Identity + '"')
    foreach ($item in $items) { $plan.Add('!insertmacro UpdateDeclare ' + $item.Id) }
    $plan.Add('!macro PackageGuardDirectories Prefix Create')
    $plan.Add('  StrCpy $UpdateCreateMissing ${Create}')
    foreach ($dir in ($directories | Sort-Object Length)) {
        $plan.Add('  StrCpy $0 "$INSTDIR\' + $dir + '"')
        $plan.Add('  Call ${Prefix}UpdateGuardDirectory')
        $plan.Add('  ${If} $UpdatePathFailed == 1')
        $plan.Add('    StrCpy $UpdateFailed 1')
        $plan.Add('  ${EndIf}')
    }
    $plan.Add('  StrCpy $UpdateCreateMissing 0')
    $plan.Add('!macroend')
    $plan.Add('!macro PackageExtract')
    $plan.Add('  SetOutPath "$TransactionRoot\stage"')
    foreach ($item in $items) {
        $plan.Add('  ClearErrors')
        if ($item.Source) { $plan.Add('  File /oname=' + $item.Id + ' "' + $item.Source + '"') }
        else { $plan.Add('  WriteUninstaller "$TransactionRoot\stage\' + $item.Id + '"') }
        $plan.Add('  ${If} ${Errors}')
        $plan.Add('    StrCpy $UpdateFailed 1')
        $plan.Add('  ${EndIf}')
    }
    $plan.Add('!macroend')
    $plan.Add('!macro PackageLease Prefix Staged')
    foreach ($item in $items) {
        $plan.Add('  ${If} ${Staged} == 1')
        $plan.Add('    StrCpy $0 "$TransactionRoot\stage\' + $item.Id + '"')
        $plan.Add('  ${Else}')
        $plan.Add('    StrCpy $0 "$INSTDIR\' + $item.Relative + '"')
        $plan.Add('  ${EndIf}')
        $plan.Add('  Call ${Prefix}LeaseProductFile')
    }
    $plan.Add('!macroend')
    $plan.Add('!macro PackageReplace')
    foreach ($item in $items) {
        $plan.Add('  !insertmacro UpdateJournaledReplace ' + $item.Id + ' "$TransactionRoot\stage\' + $item.Id + '" "$INSTDIR\' + $item.Relative + '" "$TransactionRoot\backup\' + $item.Id + '"')
    }
    $plan.Add('!macroend')
    $reverse = @($items); [Array]::Reverse($reverse)
    $plan.Add('!macro PackageRecover')
    foreach ($item in $reverse) {
        $plan.Add('  !insertmacro UpdateRecoverFile "$TransactionRoot\stage\' + $item.Id + '" "$INSTDIR\' + $item.Relative + '" "$TransactionRoot\backup\' + $item.Id + '" $TransactionCommitted')
    }
    $plan.Add('!macroend')
    $plan.Add('!macro PackageCleanStage')
    foreach ($item in $items) { $plan.Add('  Delete "$TransactionRoot\stage\' + $item.Id + '"') }
    $plan.Add('!macroend')
    $plan.Add('!macro PackageDelete')
    foreach ($item in $items | Where-Object { $_.Source }) {
        $plan.Add('  ClearErrors')
        $plan.Add('  Delete "$INSTDIR\' + $item.Relative + '"')
        $plan.Add('  ${If} ${Errors}')
        $plan.Add('    StrCpy $UpdateFailed 1')
        $plan.Add('  ${EndIf}')
    }
    $plan.Add('!macroend')
    $plan.Add('!macro PackageRemoveDirectories')
    foreach ($dir in ($directories | Sort-Object Length -Descending)) { $plan.Add('  RMDir "$INSTDIR\' + $dir + '"') }
    $plan.Add('!macroend')
    [IO.File]::WriteAllLines((Join-Path $Output 'package-plan.nsh'), $plan, [Text.UTF8Encoding]::new($false))
}
