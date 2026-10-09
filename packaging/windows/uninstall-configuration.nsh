!ifndef USB_VAULT_UNINSTALL_CONFIGURATION
!define USB_VAULT_UNINSTALL_CONFIGURATION

; Compile-time overrides are used only by isolated installer fixtures.
!ifndef ProductConfigurationDirectory
  !define ProductConfigurationDirectory "$LOCALAPPDATA\usb-vault"
!endif
!ifndef ProductCredentialPrefix
  !define ProductCredentialPrefix "usb-vault-vikey-dev"
!endif
Var UninstallDeleteConfiguration
Var UninstallConfigurationPresent

!macro DeleteProductCredential Suffix
  System::Call 'advapi32::CredDeleteW(w "${ProductCredentialPrefix}-${Suffix}", i 1, i 0) i .r0 ?e'
  Pop $1
  ${If} $0 == 0
  ${AndIf} $1 != 1168
    StrCpy $UpdateFailed 1
  ${EndIf}
!macroend

Function un.CleanUserConfiguration
  ${If} $UninstallDeleteConfiguration != 1
    Return
  ${EndIf}
  StrCpy $UninstallConfigurationPresent 0
  System::Call 'kernel32::GetFileAttributesW(w "${ProductConfigurationDirectory}") i .r0 ?e'
  Pop $1
  ${If} $0 != -1
    StrCpy $0 "${ProductConfigurationDirectory}"
    Call un.UpdateGuardDirectory
    ${If} $UpdatePathFailed == 1
      StrCpy $UpdateFailed 1
      Return
    ${EndIf}
    ; Lease both known files before changing anything. Unknown files, caches,
    ; containers and directories are never traversed or recursively deleted.
    StrCpy $0 "${ProductConfigurationDirectory}\settings.txt"
    Call un.LeaseProductFile
    StrCpy $0 "${ProductConfigurationDirectory}\container_index.bin"
    Call un.LeaseProductFile
    ${If} $UpdateFailed == 1
      Return
    ${EndIf}
    StrCpy $UninstallConfigurationPresent 1
  ${ElseIf} $1 != 2
  ${AndIf} $1 != 3
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
  ; Do not enumerate credentials or read their contents. Missing is success;
  ; access errors retain application registration so uninstall can be retried.
  !insertmacro DeleteProductCredential "credentials"
  !insertmacro DeleteProductCredential "user-sn"
  !insertmacro DeleteProductCredential "api-key"
  !insertmacro DeleteProductCredential "user-pin"
  ${If} $UpdateFailed == 1
    Return
  ${EndIf}
  ${If} $UninstallConfigurationPresent == 1
    ClearErrors
    Delete "${ProductConfigurationDirectory}\settings.txt"
    Delete "${ProductConfigurationDirectory}\container_index.bin"
    ${If} ${Errors}
      StrCpy $UpdateFailed 1
    ${EndIf}
  ${EndIf}
FunctionEnd
!endif
