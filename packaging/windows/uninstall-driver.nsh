!ifndef USB_VAULT_UNINSTALL_DRIVER
!define USB_VAULT_UNINSTALL_DRIVER
Var UninstallDeleteDriver
Var UninstallDriverFailed
Function un.CleanVeraCryptDriver
  StrCpy $UninstallDriverFailed 0
  ${If} $UninstallDeleteDriver != 1
    Return
  ${EndIf}
  DetailPrint "正在请求卸载共享 VeraCrypt 驱动…"
  !ifdef UninstallCleanupTest
    !insertmacro ${UninstallCleanupTest} '--uninstall-veracrypt'
  !else
    InitPluginsDir
    ClearErrors
    SetOutPath "$PLUGINSDIR"
    File "${SetupHelper}"
    ${If} ${Errors}
      StrCpy $UninstallDriverFailed 1
      StrCpy $UpdateFailed 1
      Return
    ${EndIf}
    nsExec::ExecToStack '"$PLUGINSDIR\usb_vault_setup_helper.exe" --uninstall-veracrypt'
    Pop $0
    Pop $1
  !endif
  ${If} $0 != 0
    StrCpy $UninstallDriverFailed 1
    StrCpy $UpdateFailed 1
  ${EndIf}
FunctionEnd
!endif
