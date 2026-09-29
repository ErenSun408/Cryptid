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
!define MUI_WELCOMEPAGE_TEXT "该向导将安装 USB Vault。所需的 VeraCrypt 驱动已包含在安装包内，无需另外安装 VeraCrypt 软件；向导会自动检测并按需安装或修复驱动。$\r$\n$\r$\n安装系统组件时会请求管理员授权。卸载 USB Vault 会保留共享组件、凭据、设置和容器文件。"
!define MUI_PAGE_CUSTOMFUNCTION_SHOW WelcomePageShow
!insertmacro MUI_PAGE_WELCOME

Page custom DependencyPageCreate DependencyPageLeave

!define MUI_LICENSEPAGE_CHECKBOX
!define MUI_LICENSEPAGE_CHECKBOX_TEXT "我已阅读并接受 USB Vault 软件包及第三方组件说明"
!define MUI_LICENSEPAGE_TEXT_TOP "请阅读软件包说明和第三方组件条款。"
!insertmacro MUI_PAGE_LICENSE "${PayloadDirectory}\licenses\THIRD-PARTY-NOTICES.txt"

!define MUI_PAGE_CUSTOMFUNCTION_PRE VeraCryptLicensePre
!define MUI_LICENSEPAGE_CHECKBOX_TEXT "我已阅读并接受 VeraCrypt License 的全部条款"
!define MUI_LICENSEPAGE_TEXT_TOP "USB Vault 将使用包内的 VeraCrypt 驱动完成安装或修复。请阅读完整许可；不接受则不能继续安装。"
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
  ClearErrors
  SetOutPath "$INSTDIR"
  ${If} ${Errors}
    MessageBox MB_ICONSTOP "无法进入 USB Vault 安装目录。错误码：1001。请检查目录是否存在及访问权限，然后从安装目录重新打开应用。"
    Return
  ${EndIf}
  Exec '$\"$INSTDIR\USB Vault.exe$\"'
  ${If} ${Errors}
    MessageBox MB_ICONSTOP "无法启动 USB Vault。错误码：1001。请确认程序文件完整且未被安全软件阻止，然后从安装目录重新打开应用。"
  ${EndIf}
FunctionEnd

Var WebViewState
Var VeraCryptState
Var WebViewCheck
Var WebViewSelection
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
  !insertmacro MUI_HEADER_TEXT "运行组件" "检查 USB Vault 所需的组件状态"
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 24u "VeraCrypt 驱动已内置，将按需自动安装或修复。缺少 WebView2 时请保留勾选；已有兼容组件会直接复用。"
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
    ${NSD_CreateLabel} 8u 56u 94% 24u "内置 VeraCrypt 驱动：系统驱动已就绪，将直接复用。"
  ${ElseIf} $VeraCryptState == 10
    ${NSD_CreateLabel} 8u 56u 94% 24u "内置 VeraCrypt 驱动：系统尚未安装，继续后将自动安装。"
  ${ElseIf} $VeraCryptState == 13
    ${NSD_CreateLabel} 8u 56u 94% 24u "内置 VeraCrypt 驱动：系统驱动尚未运行，继续后将自动修复启动。"
  ${ElseIf} $VeraCryptState == 11
    ${NSD_CreateLabel} 8u 56u 94% 24u "内置 VeraCrypt 驱动：现有系统驱动配置不兼容，无法继续。"
  ${Else}
    ${NSD_CreateLabel} 8u 56u 94% 24u "内置 VeraCrypt 驱动：系统状态检测异常，无法继续。"
  ${EndIf}
  Pop $0

  ${NSD_CreateLabel} 8u 88u 94% 34u "无需勾选驱动安装。需要安装或修复时，后续将展示许可并请求管理员授权；系统驱动不兼容或状态不明时不会覆盖。"
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

  ${If} $VeraCryptState == 11
    MessageBox MB_ICONSTOP "现有系统驱动配置与 USB Vault 不兼容。安装包虽已包含驱动，但不能直接覆盖，请先处理现有驱动。"
    Abort
  ${ElseIf} $VeraCryptState != 0
  ${AndIf} $VeraCryptState != 10
  ${AndIf} $VeraCryptState != 13
    MessageBox MB_ICONSTOP "无法确认系统中的 VeraCrypt 驱动状态。请先处理检测错误后重新运行安装。"
    Abort
  ${EndIf}
FunctionEnd

Function VeraCryptLicensePre
  ${If} $VeraCryptState != 10
  ${AndIf} $VeraCryptState != 13
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
  File "${PayloadDirectory}\prerequisites\veracrypt-x64.sys"
  ${If} $WebViewState == 10
    !insertmacro EnsureDependency "--webview" "WebView2"
  ${EndIf}
  ${If} $VeraCryptState == 10
  ${OrIf} $VeraCryptState == 13
    !insertmacro EnsureDependency "--veracrypt" "VeraCrypt 驱动"
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
