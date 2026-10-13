!ifndef USB_VAULT_UNINSTALL_WEBVIEW
!define USB_VAULT_UNINSTALL_WEBVIEW
Var UninstallProfileFailed

!macro RunWebViewCleanup Mode
  !ifdef UninstallCleanupTest
    !insertmacro ${UninstallCleanupTest} '${Mode}'
  !else
    nsExec::ExecToStack '"$PLUGINSDIR\usb_vault_setup_helper.exe" ${Mode}'
    Pop $0
    Pop $1
  !endif
  ${If} $0 != 0
    StrCpy $UninstallProfileFailed 1
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
!macroend

Function un.CleanWebViewData
  StrCpy $UninstallProfileFailed 0
  !ifndef UninstallCleanupTest
    InitPluginsDir
    ClearErrors
    SetOutPath "$PLUGINSDIR"
    File "${SetupHelper}"
    ${If} ${Errors}
      StrCpy $UninstallProfileFailed 1
      StrCpy $UpdateFailed 1
      Return
    ${EndIf}
  !endif
  DetailPrint "正在清理 USB Vault 的运行日志和浏览器缓存…"
  !insertmacro RunWebViewCleanup '--cleanup-application-data "$INSTDIR"'
FunctionEnd
!endif
