!ifndef USB_VAULT_INSTALLER_ENGINE
!define USB_VAULT_INSTALLER_ENGINE
!define UpdateIncludeUninstaller
!include "${__FILEDIR__}\update-paths.nsh"
!include "${__FILEDIR__}\update-journal.nsh"
!include "${PackagePlan}"
!ifndef ProductMenu
  !define ProductMenu "$SMPROGRAMS\USB Vault"
!endif
!ifndef ProductWindowClass
  !define ProductWindowClass "UsbVaultMainWindow"
!endif

Var TransactionRoot
Var TransactionCommitted
Var FileLeases
Var PrivateDescriptor
Var PrivateText
Var ExistingInstall
Var RegisteredLocation
Var SetupMutex
Var TransactionGuardBase

!macro SetupCommonFunctions Prefix
Function ${Prefix}AcquireSetupMutex
  StrCpy $UpdateFailed 0
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Local\USBVault.Setup.v1") p .r0'
  StrCpy $SetupMutex $0
  ${If} $0 == 0
    StrCpy $UpdateFailed 1
  ${Else}
    System::Call 'kernel32::WaitForSingleObject(p r0, i 0) i .r1'
    ${If} $1 != 0
    ${AndIf} $1 != 128
      StrCpy $UpdateFailed 1
    ${EndIf}
  ${EndIf}
FunctionEnd

; Parents must already be guarded. A write lease detects loaded images and
; readers, prevents new image loads, and permits only our rename/delete steps.
Function ${Prefix}LeaseProductFile
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  StrCpy $R5 $0
  !insertmacro UpdateInspectFile "$R5" $R4
  ${If} $R4 == 0
    Return
  ${ElseIf} $R4 != 1
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  System::Call 'kernel32::CreateFileW(w "$R5", i 0xC0010000, i 4, p 0, i 3, i 0x00200000, p 0) p .r0'
  ${If} $0 == -1
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  System::Alloc 8
  Pop $1
  ${If} $1 == 0
    System::Call 'kernel32::CloseHandle(p r0)'
    StrCpy $UpdateFailed 1
  ${Else}
    System::Call '*$1(p r0, p $FileLeases)'
    StrCpy $FileLeases $1
  ${EndIf}
FunctionEnd

Function ${Prefix}ReleaseFileLeases
  ${Do}
    ${If} $FileLeases == ""
    ${OrIf} $FileLeases == 0
      ${ExitDo}
    ${EndIf}
    StrCpy $0 $FileLeases
    System::Call '*$0(p .r1, p .r2)'
    StrCpy $FileLeases $2
    System::Call 'kernel32::CloseHandle(p r1)'
    System::Free $0
  ${Loop}
FunctionEnd

Function ${Prefix}ReleaseSetup
  Call ${Prefix}ReleaseFileLeases
  Call ${Prefix}UpdateReleasePaths
  ${If} $SetupMutex != ""
  ${AndIf} $SetupMutex != 0
    System::Call 'kernel32::ReleaseMutex(p $SetupMutex)'
    System::Call 'kernel32::CloseHandle(p $SetupMutex)'
    StrCpy $SetupMutex ""
  ${EndIf}
FunctionEnd
!macroend
!insertmacro SetupCommonFunctions ""
!insertmacro SetupCommonFunctions "un."
!include "${__FILEDIR__}\uninstall-configuration.nsh"

; Build a protected descriptor owned by the actual token user. Restrict the
; transaction tree to that user and SYSTEM, with inherited child permissions.
Function BuildPrivateSecurity
  StrCpy $PrivateDescriptor 0
  StrCpy $PrivateText ""
  System::Call 'kernel32::GetCurrentProcess() p .r0'
  System::Call 'advapi32::OpenProcessToken(p r0, i 8, *p .r1) i .r2'
  ${If} $2 == 0
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  System::Call 'advapi32::GetTokenInformation(p r1, i 1, p 0, i 0, *i .r2)'
  System::Alloc $2
  Pop $3
  ${If} $3 == 0
    StrCpy $UpdateFailed 1
  ${Else}
    System::Call 'advapi32::GetTokenInformation(p r1, i 1, p r3, i r2, *i .r2) i .r4'
    ${If} $4 == 0
      StrCpy $UpdateFailed 1
    ${Else}
      System::Call '*$3(p .r4)'
      System::Call 'advapi32::ConvertSidToStringSidW(p r4, *p .r5) i .r6'
      ${If} $6 == 0
        StrCpy $UpdateFailed 1
      ${Else}
        System::Call 'kernel32::lstrcpynW(w .r6, p r5, i ${NSIS_MAX_STRLEN})'
        System::Call 'kernel32::LocalFree(p r5)'
        StrCpy $6 "O:$6D:P(A;OICI;FA;;;$6)(A;OICI;FA;;;SY)"
        System::Call 'advapi32::ConvertStringSecurityDescriptorToSecurityDescriptorW(w r6, i 1, *p .r7, p 0) i .r8'
        ${If} $8 == 0
          StrCpy $UpdateFailed 1
        ${Else}
          StrCpy $PrivateDescriptor $7
          System::Call 'advapi32::ConvertSecurityDescriptorToStringSecurityDescriptorW(p r7, i 1, i 5, *p .r8, p 0) i .r9'
          ${If} $9 == 0
            StrCpy $UpdateFailed 1
          ${Else}
            System::Call 'kernel32::lstrcpynW(w .r9, p r8, i ${NSIS_MAX_STRLEN})'
            StrCpy $PrivateText $9
            System::Call 'kernel32::LocalFree(p r8)'
          ${EndIf}
        ${EndIf}
      ${EndIf}
    ${EndIf}
    System::Free $3
  ${EndIf}
  System::Call 'kernel32::CloseHandle(p r1)'
FunctionEnd

Function VerifyPrivateSecurity
  System::Call 'advapi32::GetNamedSecurityInfoW(w "$TransactionRoot", i 1, i 5, p 0, p 0, p 0, p 0, *p .r0) i .r1'
  ${If} $1 != 0
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  System::Call 'advapi32::ConvertSecurityDescriptorToStringSecurityDescriptorW(p r0, i 1, i 5, *p .r1, p 0) i .r2'
  ${If} $2 == 0
    StrCpy $UpdateFailed 1
  ${Else}
    System::Call 'kernel32::lstrcpynW(w .r2, p r1, i ${NSIS_MAX_STRLEN})'
    ${If} $2 != $PrivateText
      StrCpy $UpdateFailed 1
    ${EndIf}
    System::Call 'kernel32::LocalFree(p r1)'
  ${EndIf}
  System::Call 'kernel32::LocalFree(p r0)'
FunctionEnd

; This function converges metadata AFTER the file commit. Any failure retains
; the transaction and its committed marker so the same package can finish it.
Function RegisterProduct
  ClearErrors
  ; NSIS stores $OUTDIR as each shortcut's working directory. Extraction
  ; leaves it in the transaction tree, which is removed before first launch.
  SetOutPath "$INSTDIR"
  CreateDirectory "${ProductMenu}"
  CreateShortcut "${ProductMenu}\USB Vault.lnk" "$INSTDIR\USB Vault.exe"
  CreateShortcut "${ProductMenu}\卸载 USB Vault.lnk" "$INSTDIR\卸载 USB Vault.exe"
  WriteRegStr HKCU "${UninstallKey}" "DisplayName" "USB Vault"
  WriteRegStr HKCU "${UninstallKey}" "DisplayVersion" "0.2.0"
  WriteRegStr HKCU "${UninstallKey}" "UninstallString" '$\"$INSTDIR\卸载 USB Vault.exe$\"'
  WriteRegStr HKCU "${UninstallKey}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${UninstallKey}" "NoModify" 1
  WriteRegDWORD HKCU "${UninstallKey}" "NoRepair" 1
  ${If} ${Errors}
    StrCpy $UpdateFailed 1
  ${EndIf}
FunctionEnd

Function RecoverPackage
  StrCpy $UpdateRecoveryFailed 0
  !insertmacro UpdateLoadCommit "$TransactionRoot\committed" $TransactionCommitted
  ${If} $UpdateRecoveryFailed != 1
  ${AndIf} $TransactionCommitted == 1
    Call RegisterProduct
    ${If} $UpdateFailed == 1
      StrCpy $UpdateRecoveryFailed 1
    ${EndIf}
  ${EndIf}
  !insertmacro PackageRecover
  ${If} $UpdateRecoveryFailed == 1
    StrCpy $UpdateFailed 1
  ${EndIf}
FunctionEnd

Function CheckTransactionNames
  ClearErrors
  FindFirst $0 $1 "$TransactionRoot\*"
  ${If} ${Errors}
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  ${Do}
    ${If} $1 != "."
    ${AndIf} $1 != ".."
    ${AndIf} $1 != "stage"
    ${AndIf} $1 != "backup"
    ${AndIf} $1 != "committed"
    ${AndIf} $1 != "owner-${PackageIdentity}"
      StrCpy $UpdateFailed 1
    ${EndIf}
    ClearErrors
    FindNext $0 $1
    ${If} ${Errors}
      ${ExitDo}
    ${EndIf}
  ${Loop}
  FindClose $0
FunctionEnd

; Recheck after the directory page and after pinning the selected root.
Function CheckInstallTarget
  ${If} $ExistingInstall == 1
    ClearErrors
    ReadRegStr $0 HKCU "${UninstallKey}" "InstallLocation"
    ${If} ${Errors}
      StrCpy $UpdateFailed 1
    ${Else}
      System::Call 'kernel32::CompareStringOrdinal(w r0, i -1, w "$INSTDIR", i -1, i 1) i .r1'
      ${If} $1 != 2
        StrCpy $UpdateFailed 1
      ${EndIf}
    ${EndIf}
    Return
  ${EndIf}
  !insertmacro UpdateReadMarker "$TransactionRoot\owner-${PackageIdentity}" $R6
  ${If} $R6 == 827741442
    Return
  ${EndIf}
  ClearErrors
  FindFirst $0 $1 "$INSTDIR\*"
  ${If} ${Errors}
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  ${Do}
    ${If} $1 != "."
    ${AndIf} $1 != ".."
      StrCpy $UpdateFailed 1
    ${EndIf}
    ClearErrors
    FindNext $0 $1
    ${If} ${Errors}
      ${ExitDo}
    ${EndIf}
  ${Loop}
  FindClose $0
FunctionEnd

Function CleanTransaction
  ; No wildcard deletion. Leave unknown entries and recovery identity intact.
  Call CheckTransactionNames
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  Call ReleaseFileLeases
  !insertmacro PackageCleanStage
  SetOutPath "$TEMP"
  StrCpy $DirectoryReleaseStop $TransactionGuardBase
  Call ReleaseDirectorySuffix
  ClearErrors
  RMDir "$TransactionRoot\stage"
  RMDir "$TransactionRoot\backup"
  ${If} ${Errors}
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  Delete "$TransactionRoot\committed"
  Delete "$TransactionRoot\owner-${PackageIdentity}"
  ; Release just the transaction directory, keeping its ancestry pinned.
  StrCpy $0 $UpdateDirectoryHandles
  System::Call '*$0(p .r1, p .r2)'
  StrCpy $UpdateDirectoryHandles $2
  System::Call 'kernel32::CloseHandle(p r1)'
  System::Free $0
  RMDir "$TransactionRoot"
  ${If} ${Errors}
    StrCpy $UpdateFailed 1
  ${EndIf}
FunctionEnd

Function InstallPackage
  StrCpy $UpdateFailed 0
  StrCpy $TransactionRoot "$INSTDIR\.usb-vault-update"
  FindWindow $0 "${ProductWindowClass}"
  ${If} $0 != 0
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  StrCpy $UpdateCreateMissing 1
  StrCpy $0 $INSTDIR
  Call UpdateGuardDirectory
  StrCpy $UpdateCreateMissing 0
  ${If} $UpdatePathFailed == 1
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  Call CheckInstallTarget
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  StrCpy $0 "$INSTDIR\.usb-vault-setup.lock"
  Call UpdateAcquireLock
  ${If} $UpdatePathFailed == 1
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  !insertmacro PackageGuardDirectories "" 1
  !insertmacro PackageLease "" 0
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  Call BuildPrivateSecurity
  ${If} $UpdateFailed == 1
    ${If} $PrivateDescriptor != 0
      System::Call 'kernel32::LocalFree(p $PrivateDescriptor)'
      StrCpy $PrivateDescriptor 0
    ${EndIf}
    Return
  ${EndIf}
  System::Call '*(i 12, p $PrivateDescriptor, i 0) p .r0'
  System::Call 'kernel32::CreateDirectoryW(w "$TransactionRoot", p r0) i .r1 ?e'
  Pop $2
  System::Free $0
  ${If} $1 != 0
    !insertmacro UpdateWriteMarker "$TransactionRoot\owner-${PackageIdentity}" 827741442
  ${ElseIf} $2 != 183
    StrCpy $UpdateFailed 1
  ${EndIf}
  StrCpy $0 $TransactionRoot
  Call UpdateGuardDirectory
  ${If} $UpdatePathFailed == 1
    StrCpy $UpdateFailed 1
  ${EndIf}
  ${If} $UpdateFailed != 1
    Call VerifyPrivateSecurity
    !insertmacro UpdateReadMarker "$TransactionRoot\owner-${PackageIdentity}" $R6
    ${If} $R6 != 827741442
      StrCpy $UpdateFailed 1
    ${EndIf}
  ${EndIf}
  System::Call 'kernel32::LocalFree(p $PrivateDescriptor)'
  StrCpy $PrivateDescriptor 0
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  Call CheckTransactionNames
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  StrCpy $TransactionGuardBase $UpdateDirectoryHandles
  StrCpy $UpdateCreateMissing 1
  StrCpy $0 "$TransactionRoot\stage"
  Call UpdateGuardDirectory
  ${If} $UpdatePathFailed == 1
    StrCpy $UpdateFailed 1
  ${EndIf}
  StrCpy $0 "$TransactionRoot\backup"
  Call UpdateGuardDirectory
  StrCpy $UpdateCreateMissing 0
  ${If} $UpdatePathFailed == 1
    StrCpy $UpdateFailed 1
  ${EndIf}
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  Call RecoverPackage
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  ${If} $TransactionCommitted == 1
    Call CleanTransaction
    Return
  ${EndIf}
  ; Recovery may have moved a leased replacement back into staging. Release
  ; those handles and lease the restored targets before extracting again.
  Call ReleaseFileLeases
  !insertmacro PackageLease "" 0
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  !insertmacro PackageExtract
  !insertmacro PackageLease "" 1
  ${If} $UpdateFailed != 1
    !insertmacro PackageReplace
  ${EndIf}
  ${If} $UpdateFailed != 1
    !insertmacro UpdateWriteMarker "$TransactionRoot\committed" 827741442
    !ifdef InstallerTestCheckpoint
      !insertmacro ${InstallerTestCheckpoint} files-committed
    !endif
  ${EndIf}
  StrCpy $R8 $UpdateFailed
  StrCpy $UpdateFailed 0
  Call RecoverPackage
  ${If} $UpdateFailed != 1
    Call CleanTransaction
  ${EndIf}
  ${If} $R8 == 1
    StrCpy $UpdateFailed 1
  ${EndIf}
FunctionEnd
Function un.UninstallPackage
  ; Explicit files only. Never recursively delete an application directory
  ; that may contain container files or an encrypted user index.
  FindWindow $0 "${ProductWindowClass}"
  ${If} $0 != 0
    Goto engine_uninstall_failed
  ${EndIf}
  StrCpy $0 $INSTDIR
  Call un.UpdateGuardDirectory
  ${If} $UpdatePathFailed == 1
    Goto engine_uninstall_failed
  ${EndIf}
  StrCpy $0 "$INSTDIR\.usb-vault-setup.lock"
  Call un.UpdateAcquireLock
  ${If} $UpdatePathFailed == 1
    Goto engine_uninstall_failed
  ${EndIf}
  System::Call 'kernel32::GetFileAttributesW(w "$INSTDIR\.usb-vault-update") i .r0 ?e'
  Pop $1
  ${If} $0 != -1
    Goto engine_uninstall_failed
  ${ElseIf} $1 != 2
  ${AndIf} $1 != 3
    Goto engine_uninstall_failed
  ${EndIf}
  !insertmacro PackageGuardDirectories "un." 1
  !insertmacro PackageLease "un." 0
  ${If} $UpdateFailed == 1
    Goto engine_uninstall_failed
  ${EndIf}
  Call un.CleanUserConfiguration
  ${If} $UpdateFailed == 1
    Goto engine_uninstall_failed
  ${EndIf}
  !insertmacro PackageDelete
  Call un.ReleaseFileLeases
  ${If} $UpdateFailed == 1
    Goto engine_uninstall_failed
  ${EndIf}
  ClearErrors
  Delete "${ProductMenu}\USB Vault.lnk"
  Delete "${ProductMenu}\卸载 USB Vault.lnk"
  ; Menu directories containing unrelated shortcuts are preserved.
  DeleteRegKey HKCU "${UninstallKey}"
  ${If} ${Errors}
    Goto engine_uninstall_failed
  ${EndIf}
  Delete "$INSTDIR\卸载 USB Vault.exe"
  Call un.ReleaseSetup
  ${If} $UninstallDeleteConfiguration == 1
    ; Remove only an empty configuration directory, after releasing its guards.
    RMDir "${ProductConfigurationDirectory}"
  ${EndIf}
  !insertmacro PackageRemoveDirectories
  RMDir "$INSTDIR"
  Goto engine_uninstall_done
engine_uninstall_failed:
  Call un.ReleaseSetup
  StrCpy $UpdateFailed 1
  SetErrorLevel 1603
engine_uninstall_done:
FunctionEnd
!endif
