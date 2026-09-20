; -*- coding: utf-8 -*-
Unicode true
!ifdef UpdateTestCheckpoint
  !error "Fault injection is forbidden in the product installer"
!endif
!ifdef InstallerTestCheckpoint
  !error "Fault injection is forbidden in the product installer"
!endif
!include MUI2.nsh
!include LogicLib.nsh
!include nsDialogs.nsh
!include FileFunc.nsh
!include x64.nsh
!include "${__FILEDIR__}\installation-record.nsh"

!ifndef PayloadDirectory
  !error "Use tools/build-windows-installer.ps1 with a verified package"
!endif
Name "USB Vault"
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
!include "${__FILEDIR__}\installer-engine.nsh"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "该向导将完成 USB Vault、WebView2 Runtime 和VeraCrypt的检测与安装。已兼容的依赖将自动跳过。$\r$\n$\r$\n安装系统依赖时会请求管理员授权。卸载 USB Vault 会保留共享依赖、凭据、设置和容器文件。"
!define MUI_PAGE_CUSTOMFUNCTION_SHOW WelcomePageShow
!insertmacro MUI_PAGE_WELCOME

Page custom DependencyPageCreate DependencyPageLeave

!define MUI_LICENSEPAGE_CHECKBOX
!define MUI_LICENSEPAGE_CHECKBOX_TEXT "我已阅读并接受 USB Vault 软件包及第三方组件说明"
!define MUI_LICENSEPAGE_TEXT_TOP "请阅读软件包说明和第三方组件条款。"
!insertmacro MUI_PAGE_LICENSE "${PayloadDirectory}\licenses\THIRD-PARTY-NOTICES.txt"

!define MUI_PAGE_CUSTOMFUNCTION_PRE VeraCryptLicensePre
!define MUI_LICENSEPAGE_CHECKBOX_TEXT "我已阅读并接受 VeraCrypt License 的全部条款"
!define MUI_LICENSEPAGE_TEXT_TOP "将安装 VeraCrypt 1.26.29。请阅读完整许可；不接受则不能继续安装。"
!insertmacro MUI_PAGE_LICENSE "${PayloadDirectory}\licenses\veracrypt\License.txt"

!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ValidateInstallDirectory
!define MUI_PAGE_CUSTOMFUNCTION_PRE InstallDirectoryPre
!define MUI_DIRECTORYPAGE_TEXT_TOP "请选择 USB Vault 的最终安装文件夹。已有安装将在原位置更新，保留凭据、设置和容器。路径请不要包含中文。"
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\USB Vault.exe"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchInstalledApplication
!define MUI_FINISHPAGE_RUN_TEXT "打开 USB Vault"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"

Function LaunchInstalledApplication
  ; Cleanup leaves $OUTDIR at $TEMP. Match a direct application launch and
  ; the installed shortcuts instead of inheriting the installer's workspace.
  SetOutPath "$INSTDIR"
  Exec '$\"$INSTDIR\USB Vault.exe$\"'
FunctionEnd

Var WebViewState
Var VeraCryptState
Var WebViewCheck
Var VeraCryptCheck
Var WebViewSelection
Var VeraCryptSelection
Var WelcomeTitleFont
Var WelcomeTextFont

Function WelcomePageShow
  CreateFont $WelcomeTitleFont "Microsoft YaHei UI" 12 700
  SendMessage $mui.WelcomePage.Title ${WM_SETFONT} $WelcomeTitleFont 1
  CreateFont $WelcomeTextFont "Microsoft YaHei UI" 9 400
  SendMessage $mui.WelcomePage.Text ${WM_SETFONT} $WelcomeTextFont 1
FunctionEnd

Function .onInit
  IfSilent 0 +3
    SetErrorLevel 64
    Quit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "当前安装包需要 Windows x64。"
    Abort
  ${EndIf}
  Call AcquireSetupMutex
  ${If} $UpdateFailed == 1
    MessageBox MB_ICONSTOP "另一个 USB Vault 安装或卸载程序正在运行，请关闭后重试。"
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
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\USB Vault"
  ; Configuration is a page inside the product window now, so the product
  ; window is the only one to look for.
  FindWindow $0 "UsbVaultMainWindow"
  ${If} $0 != 0
    MessageBox MB_ICONSTOP "请正常关闭 USB Vault 后重新安装。"
    Abort
  ${EndIf}
  ClearErrors
  ReadRegStr $0 HKCU "${UninstallKey}" "InstallLocation"
  ${IfNot} ${Errors}
    ${If} $0 != ""
      !insertmacro ProbeRecordedInstallation
      ${If} $1 == 10
        StrCpy $RegisteredLocation $0
        StrCpy $INSTDIR $0
        StrCpy $0 $INSTDIR
        Call UpdateGuardDirectory
        Call UpdateReleasePaths
        ${If} $UpdatePathFailed == 1
          MessageBox MB_ICONSTOP "原安装位置不是可访问的本机普通目录，不能安全更新。"
          Abort
        ${EndIf}
        StrCpy $ExistingInstall 1
        !insertmacro UpdateInspectFile "$INSTDIR\USB Vault.exe" $R6
        ${If} $R6 == -1
          MessageBox MB_ICONSTOP "无法确认原主程序的文件状态，不能安全更新。"
          Abort
        ${ElseIf} $R6 == 0
          StrCpy $R5 "主程序已缺失。将根据原安装记录修复："
        ${Else}
          StrCpy $R5 "检测到当前设备已安装 USB Vault。将更新原位置："
        ${EndIf}
        MessageBox MB_ICONQUESTION|MB_OKCANCEL "$R5$\r$\n$INSTDIR$\r$\n$\r$\n凭据、设置、索引和容器文件会保留。继续吗？" IDOK +2
        Abort
      ${ElseIf} $1 == 20
        MessageBox MB_ICONSTOP "无法确认 USB Vault 原安装位置的状态，请检查路径和访问权限后重试。"
        Abort
      ${EndIf}
    ${EndIf}
  ${EndIf}
FunctionEnd

Function InstallDirectoryPre
  ${If} $ExistingInstall == 1
    StrCpy $INSTDIR $RegisteredLocation
    Abort
  ${EndIf}
FunctionEnd

Function .onGUIEnd
  Call ReleaseSetup
FunctionEnd

Function ValidateInstallDirectory
  ; The NSIS GetFullPathName instruction returns an empty string when the
  ; selected final directory does not exist yet. Use the Windows API so a new
  ; installation directory can be normalized before it is created.
  StrCpy $0 "$INSTDIR"
  ${GetRoot} "$0" $1
  ${If} $1 == ""
    MessageBox MB_ICONEXCLAMATION "请选择本机固定磁盘上的有效绝对路径。"
    Abort
  ${EndIf}
  StrCpy $2 "$0" 1 2
  ${If} $2 != "\"
    MessageBox MB_ICONEXCLAMATION "请选择本机固定磁盘上的有效绝对路径。"
    Abort
  ${EndIf}
  System::Call 'kernel32::GetFullPathNameW(w r0, i ${NSIS_MAX_STRLEN}, w .r2, p 0) i .r3'
  ${If} $3 == 0
  ${OrIf} $3 >= ${NSIS_MAX_STRLEN}
    MessageBox MB_ICONEXCLAMATION "安装路径无效或过长，请选择其他位置。"
    Abort
  ${EndIf}
  StrCpy $INSTDIR $2
  ${If} $ExistingInstall == 1
    System::Call 'kernel32::CompareStringOrdinal(w "$INSTDIR", i -1, w "$RegisteredLocation", i -1, i 1) i .r0'
    ${If} $0 != 2
      MessageBox MB_ICONSTOP "更新必须使用原安装位置。"
      Abort
    ${EndIf}
    Return
  ${EndIf}
  ${GetRoot} "$INSTDIR" $1
  ${If} $INSTDIR == "$1\"
    MessageBox MB_ICONEXCLAMATION "不能直接安装到磁盘根目录。请选择或输入一个名为“USB Vault”的安装文件夹。"
    Abort
  ${EndIf}
  StrCpy $2 "$1\"
  System::Call 'kernel32::GetDriveTypeW(w r2) i .r3'
  ${If} $3 != 3
    MessageBox MB_ICONEXCLAMATION "安装位置必须位于本机固定磁盘，不能使用网络盘或可移动磁盘。"
    Abort
  ${EndIf}
  ; Recovery of a new installation may precede registry publication.
  !insertmacro UpdateReadMarker "$INSTDIR\.usb-vault-update\owner-${PackageIdentity}" $R6
  ${If} $R6 == 827741442
    Return
  ${EndIf}

  ; The final installation directory must be new or empty.
  ClearErrors
  FindFirst $3 $4 "$INSTDIR\*"
  IfErrors directory_valid
directory_scan:
  StrCmp $4 "." directory_next
  StrCmp $4 ".." directory_next
  FindClose $3
  MessageBox MB_ICONEXCLAMATION "所选安装文件夹已存在且不是空目录。请选择其他位置，或先清理该文件夹。"
  Abort
directory_next:
  ClearErrors
  FindNext $3 $4
  IfErrors directory_scan_done
  Goto directory_scan
directory_scan_done:
  FindClose $3
directory_valid:
  ClearErrors
FunctionEnd

Function DependencyPageCreate
  !insertmacro MUI_HEADER_TEXT "安装组件" "确认 USB Vault 所需的共享依赖"
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 24u "缺少的依赖已默认勾选，缺少任一必需项将无法继续。已安装的兼容版本不会被覆盖。"
  Pop $0

  ${If} $WebViewState == 0
    ${NSD_CreateCheckbox} 8u 34u 94% 14u "WebView2 Runtime（已安装）"
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
    ${NSD_CreateCheckbox} 8u 56u 94% 14u "VeraCrypt 1.26.29（已安装）"
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

  ${NSD_CreateLabel} 8u 84u 94% 34u "勾选安装即请求以管理员权限安装对应发布者的原版组件。"
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

  Call InstallPackage
  Call ReleaseSetup
  ${If} $UpdateFailed == 1
    MessageBox MB_ICONSTOP "安装／更新未完成。请关闭 USB Vault，检查磁盘空间、权限或文件占用后，重新运行同一个安装包。恢复记录及备份会保留；不要手动删除 .usb-vault-update 文件夹。"
    SetErrorLevel 1603
    Quit
  ${EndIf}
SectionEnd

Function .onInstSuccess
  IfRebootFlag 0 +2
    SetErrorLevel 3010
FunctionEnd

Function un.onInit
  Call un.AcquireSetupMutex
  ${If} $UpdateFailed == 1
    MessageBox MB_ICONSTOP "另一个 USB Vault 安装或卸载程序正在运行。"
    Abort
  ${EndIf}
  ; A moved uninstaller must match the actual location recorded at install.
  ClearErrors
  ReadRegStr $0 HKCU "${UninstallKey}" "InstallLocation"
  ${If} ${Errors}
    MessageBox MB_ICONSTOP "卸载程序位置无效，请从原安装目录运行。"
    Abort
  ${EndIf}
  GetFullPathName $0 "$0"
  GetFullPathName $1 "$INSTDIR"
  System::Call 'kernel32::CompareStringOrdinal(w r0, i -1, w r1, i -1, i 1) i .r2'
  ${If} $2 != 2
    MessageBox MB_ICONSTOP "卸载程序位置与安装记录不一致，请从原安装目录运行。"
    Abort
  ${EndIf}
  FindWindow $0 "UsbVaultMainWindow"
  ${If} $0 != 0
    MessageBox MB_ICONSTOP "请正常关闭 USB Vault 后再卸载。"
    Abort
  ${EndIf}
FunctionEnd

Function un.onGUIEnd
  Call un.ReleaseSetup
FunctionEnd

Section "Uninstall"
  Call un.UninstallPackage
  ${If} $UpdateFailed == 1
    MessageBox MB_ICONSTOP "卸载未完成。请关闭程序并处理文件占用；如果存在未完成的更新，请先运行对应安装包恢复。安装记录和用户数据会保留。"
  ${EndIf}
SectionEnd
