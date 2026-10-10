!ifndef USB_VAULT_UNINSTALL_WEBVIEW
!define USB_VAULT_UNINSTALL_WEBVIEW
Var UninstallDeleteWebView
Var UninstallWebViewFailed

!macro RunWebViewCleanup Mode
  !ifdef UninstallCleanupTest
    !insertmacro ${UninstallCleanupTest} '${Mode}'
  !else
    nsExec::ExecToStack '"$PLUGINSDIR\usb_vault_setup_helper.exe" ${Mode}'
    Pop $0
    Pop $1
  !endif
  ${If} $0 != 0
    StrCpy $UninstallWebViewFailed 1
    StrCpy $UpdateFailed 1
    Return
  ${EndIf}
!macroend

Function un.CleanWebViewData
  StrCpy $UninstallWebViewFailed 0
  ${If} $UninstallDeleteConfiguration != 1
  ${AndIf} $UninstallDeleteWebView != 1
    Return
  ${EndIf}
  !ifndef UninstallCleanupTest
    InitPluginsDir
    ClearErrors
    SetOutPath "$PLUGINSDIR"
    File "${SetupHelper}"
    ${If} ${Errors}
      StrCpy $UninstallWebViewFailed 1
      StrCpy $UpdateFailed 1
      Return
    ${EndIf}
  !endif
  ${If} $UninstallDeleteConfiguration == 1
    DetailPrint "正在清理 USB Vault 的浏览器数据…"
    !insertmacro RunWebViewCleanup '--cleanup-webview-profile "$INSTDIR"'
  ${EndIf}
  ${If} $UninstallDeleteWebView == 1
    DetailPrint "正在请求卸载共享 WebView2 Runtime…"
    !insertmacro RunWebViewCleanup '--uninstall-webview'
  ${EndIf}
FunctionEnd
!endif
