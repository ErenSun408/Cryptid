; -*- coding: utf-8 -*-
Unicode true
!include MUI2.nsh
!include LogicLib.nsh
!include nsDialogs.nsh
!include x64.nsh

!ifndef PayloadDirectory
  !error "Use tools/build-windows-installer.ps1 with a verified package"
!endif
Name "USB Vault（开发版）"
OutFile "${OutputFile}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\USB Vault"
CRCCheck force
; The prerequisite installers are already compressed. Non-solid zlib avoids
; exhausting the 32-bit NSIS compiler while embedding the large offline files.
SetCompressor zlib
ShowInstDetails show
ShowUninstDetails show
!define UninstallKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\USBVaultDevelopment"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "一个向导完成 USB Vault、WebView2 Runtime 和 VeraCrypt 的检测与安装。已兼容的依赖会自动跳过。$\r$\n$\r$\n安装系统依赖时会请求管理员授权，但不会再打开单独的依赖安装向导。$\r$\n$\r$\n卸载 USB Vault 会保留共享依赖、凭据、设置和容器文件。"
!insertmacro MUI_PAGE_WELCOME

Page custom DependencyPageCreate DependencyPageLeave

!define MUI_LICENSEPAGE_CHECKBOX
!define MUI_LICENSEPAGE_CHECKBOX_TEXT "我已阅读并接受 USB Vault 软件包及第三方组件说明"
!define MUI_LICENSEPAGE_TEXT_TOP "请阅读软件包说明和第三方组件条款。必须接受后才能继续。"
!insertmacro MUI_PAGE_LICENSE "${PayloadDirectory}\licenses\THIRD-PARTY-NOTICES.txt"

!define MUI_PAGE_CUSTOMFUNCTION_PRE VeraCryptLicensePre
!define MUI_LICENSEPAGE_CHECKBOX_TEXT "我已阅读并接受 VeraCrypt License 的全部条款"
!define MUI_LICENSEPAGE_TEXT_TOP "将安装 VeraCrypt 1.26.29。请阅读完整许可；不接受则不能继续安装。"
!insertmacro MUI_PAGE_LICENSE "${PayloadDirectory}\licenses\veracrypt\License.txt"

!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\usb_vault_vikey_p2_development_app.exe"
!define MUI_FINISHPAGE_RUN_TEXT "打开 USB Vault"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

Var WebViewState
Var VeraCryptState
Var WebViewCheck
Var VeraCryptCheck
Var WebViewSelection
Var VeraCryptSelection

Function .onInit
  IfSilent 0 +3
    SetErrorLevel 64
    Quit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "当前安装包需要 Windows x64。"
    Abort
  ${EndIf}
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File "${SetupHelper}"
  nsExec::ExecToStack '"$PLUGINSDIR\usb_vault_setup_helper.exe" --probe-webview'
  Pop $WebViewState
  Pop $0
  nsExec::ExecToStack '"$PLUGINSDIR\usb_vault_setup_helper.exe" --probe-veracrypt'
  Pop $VeraCryptState
  Pop $0
  ; Do not honor an arbitrary /D= target for this per-user installer.
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\USB Vault"
  FindWindow $0 "UsbVaultMainWindow"
  FindWindow $1 "UsbVaultVikeyDevelopmentCredentialsConfig"
  ${If} $0 != 0
  ${OrIf} $1 != 0
    MessageBox MB_ICONSTOP "请正常关闭 USB Vault 和配置窗口后重新安装。"
    Abort
  ${EndIf}
  IfFileExists "$INSTDIR\usb_vault_vikey_p2_development_app.exe" 0 +3
    MessageBox MB_ICONSTOP "已安装 USB Vault。此预览安装器暂不覆盖升级，请先卸载旧版（会保留用户数据），再运行安装。"
    Abort
FunctionEnd

Function DependencyPageCreate
  !insertmacro MUI_HEADER_TEXT "安装组件" "确认 USB Vault 所需的共享依赖"
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 24u "USB Vault 主应用始终安装。缺少的依赖已默认勾选；取消任一必需项将无法继续。已安装的兼容版本不会被覆盖。"
  Pop $0

  ${If} $WebViewState == 0
    ${NSD_CreateCheckbox} 8u 34u 94% 14u "WebView2 Runtime（已安装，将跳过）"
    Pop $WebViewCheck
    ${NSD_Check} $WebViewCheck
    EnableWindow $WebViewCheck 0
  ${ElseIf} $WebViewState == 10
    ${NSD_CreateCheckbox} 8u 34u 94% 14u "安装 Microsoft Edge WebView2 Runtime"
    Pop $WebViewCheck
    ${If} $WebViewSelection == ""
      StrCpy $WebViewSelection ${BST_CHECKED}
    ${EndIf}
    ${NSD_SetState} $WebViewCheck $WebViewSelection
  ${Else}
    ${NSD_CreateCheckbox} 8u 34u 94% 14u "WebView2 Runtime（检测异常，无法继续）"
    Pop $WebViewCheck
    EnableWindow $WebViewCheck 0
  ${EndIf}

  ${If} $VeraCryptState == 0
    ${NSD_CreateCheckbox} 8u 56u 94% 14u "VeraCrypt 1.26.29（已安装，将跳过）"
    Pop $VeraCryptCheck
    ${NSD_Check} $VeraCryptCheck
    EnableWindow $VeraCryptCheck 0
  ${ElseIf} $VeraCryptState == 10
    ${NSD_CreateCheckbox} 8u 56u 94% 14u "安装 VeraCrypt 1.26.29"
    Pop $VeraCryptCheck
    ${If} $VeraCryptSelection == ""
      StrCpy $VeraCryptSelection ${BST_CHECKED}
    ${EndIf}
    ${NSD_SetState} $VeraCryptCheck $VeraCryptSelection
  ${ElseIf} $VeraCryptState == 11
    ${NSD_CreateCheckbox} 8u 56u 94% 14u "VeraCrypt（现有版本与 USB Vault 不兼容）"
    Pop $VeraCryptCheck
    EnableWindow $VeraCryptCheck 0
  ${Else}
    ${NSD_CreateCheckbox} 8u 56u 94% 14u "VeraCrypt（检测异常，无法继续）"
    Pop $VeraCryptCheck
    EnableWindow $VeraCryptCheck 0
  ${EndIf}

  ${NSD_CreateLabel} 8u 84u 94% 34u "勾选安装即请求以管理员权限安装对应发布者的原版组件。下一页显示软件包说明；需要安装 VeraCrypt 时还会单独显示其完整许可。"
  Pop $0
  nsDialogs::Show
FunctionEnd

Function DependencyPageLeave
  ${If} $WebViewState == 10
    ${NSD_GetState} $WebViewCheck $WebViewSelection
    ${If} $WebViewSelection != ${BST_CHECKED}
      MessageBox MB_ICONEXCLAMATION "WebView2 Runtime 是 USB Vault 的必需组件。请勾选安装，或取消本次安装。"
      Abort
    ${EndIf}
  ${ElseIf} $WebViewState != 0
    MessageBox MB_ICONSTOP "无法确认 WebView2 Runtime 状态。请先处理检测错误后重新运行安装。"
    Abort
  ${EndIf}

  ${If} $VeraCryptState == 10
    ${NSD_GetState} $VeraCryptCheck $VeraCryptSelection
    ${If} $VeraCryptSelection != ${BST_CHECKED}
      MessageBox MB_ICONEXCLAMATION "VeraCrypt 是 USB Vault 的必需组件。请勾选安装并接受其许可，或取消本次安装。"
      Abort
    ${EndIf}
  ${ElseIf} $VeraCryptState == 11
    MessageBox MB_ICONSTOP "现有 VeraCrypt 版本与 USB Vault 要求的 1.26.29 驱动不兼容。本安装器不会自动覆盖，请先处理现有版本。"
    Abort
  ${ElseIf} $VeraCryptState != 0
    MessageBox MB_ICONSTOP "无法确认 VeraCrypt 状态。请先处理检测错误后重新运行安装。"
    Abort
  ${EndIf}
FunctionEnd

Function VeraCryptLicensePre
  ${If} $VeraCryptState != 10
    Abort
  ${EndIf}
  ${If} $VeraCryptSelection != ${BST_CHECKED}
    Abort
  ${EndIf}
FunctionEnd

!macro EnsureDependency Switch Name
  DetailPrint "正在检测或安装 ${Name}…"
  nsExec::ExecToStack '"$PLUGINSDIR\usb_vault_setup_helper.exe" ${Switch}'
  Pop $0
  Pop $1
  ${If} $0 == 20
    SetRebootFlag true
    DetailPrint "${Name} 安装成功，完成 USB Vault 安装后需要重启 Windows。"
  ${ElseIf} $0 == 21
    MessageBox MB_ICONINFORMATION "已取消 ${Name} 安装。此次尚未安装 USB Vault，可稍后重试。"
    SetErrorLevel 1602
    Quit
  ${ElseIf} $0 == 11
    MessageBox MB_ICONSTOP "现有 ${Name} 版本不兼容，未覆盖。请处理现有版本后重新运行安装。"
    SetErrorLevel 1603
    Quit
  ${ElseIf} $0 != 0
    MessageBox MB_ICONSTOP "${Name} 未就绪（代码 $0）。请确认安装已完成、授权未取消；如刚安装驱动，请重启后重试。此次尚未安装 USB Vault。"
    SetErrorLevel 1603
    Quit
  ${EndIf}
!macroend

Section "USB Vault" Main
  SetOutPath "$PLUGINSDIR\prerequisites"
  File "${PayloadDirectory}\prerequisites\MicrosoftEdgeWebView2RuntimeInstallerX64.exe"
  File "${PayloadDirectory}\prerequisites\VeraCrypt_Setup_x64_1.26.29.msi"
  ${If} $WebViewState == 10
    !insertmacro EnsureDependency "--webview" "WebView2"
  ${EndIf}
  ${If} $VeraCryptState == 10
    !insertmacro EnsureDependency "--veracrypt" "VeraCrypt"
  ${EndIf}

  ; Only compile-time enumerated package files are installed. No user index or
  ; credential data is shipped, and no standalone configuration EXE is added.
  ClearErrors
  !include "${InstallFiles}"
  IfErrors install_failed
  WriteUninstaller "$INSTDIR\卸载 USB Vault.exe"
  IfErrors install_failed
  CreateDirectory "$SMPROGRAMS\USB Vault"
  CreateShortcut "$SMPROGRAMS\USB Vault\USB Vault.lnk" "$INSTDIR\usb_vault_vikey_p2_development_app.exe"
  CreateShortcut "$SMPROGRAMS\USB Vault\卸载 USB Vault.lnk" "$INSTDIR\卸载 USB Vault.exe"
  WriteRegStr HKCU "${UninstallKey}" "DisplayName" "USB Vault（开发版）"
  WriteRegStr HKCU "${UninstallKey}" "DisplayVersion" "0.2.0"
  WriteRegStr HKCU "${UninstallKey}" "UninstallString" '$\"$INSTDIR\卸载 USB Vault.exe$\"'
  WriteRegStr HKCU "${UninstallKey}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${UninstallKey}" "NoModify" 1
  WriteRegDWORD HKCU "${UninstallKey}" "NoRepair" 1
  IfErrors install_failed
  Goto install_done
install_failed:
  MessageBox MB_ICONSTOP "安装文件或快捷方式失败，请检查权限和磁盘空间。未标记为成功；请保留现有数据并清理本次不完整安装后重试。"
  SetErrorLevel 1603
  Quit
install_done:
SectionEnd

Function .onInstSuccess
  IfRebootFlag 0 +2
    SetErrorLevel 3010
FunctionEnd

Function un.onInit
  ; A moved uninstaller must not remove files from a different directory.
  StrCmp $INSTDIR "$LOCALAPPDATA\Programs\USB Vault" +3
    MessageBox MB_ICONSTOP "卸载程序位置无效，请从原安装目录运行。"
    Abort
  FindWindow $0 "UsbVaultMainWindow"
  FindWindow $1 "UsbVaultVikeyDevelopmentCredentialsConfig"
  ${If} $0 != 0
  ${OrIf} $1 != 0
    MessageBox MB_ICONSTOP "请正常关闭 USB Vault 后再卸载。"
    Abort
  ${EndIf}
FunctionEnd

Section "Uninstall"
  ; Explicit files only. Never recursively delete an application directory
  ; that may contain container files or an encrypted user index.
  !include "${UninstallFiles}"
  Delete "$INSTDIR\卸载 USB Vault.exe"
  Delete "$SMPROGRAMS\USB Vault\USB Vault.lnk"
  Delete "$SMPROGRAMS\USB Vault\卸载 USB Vault.lnk"
  RMDir "$SMPROGRAMS\USB Vault"
  DeleteRegKey HKCU "${UninstallKey}"
  RMDir "$INSTDIR"
SectionEnd
