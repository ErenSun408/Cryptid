!ifndef USB_VAULT_DRIVER_DIAGNOSTICS
!define USB_VAULT_DRIVER_DIAGNOSTICS
!include LogicLib.nsh
!include WordFunc.nsh
!insertmacro WordFind

; Input is the helper's fixed ASCII numeric record, never exception text.
; This display-only record cannot select files, commands or installation paths.
Function FormatDriverDiagnostic
  Exch $R0
  Push $R1
  Push $R2
  Push $R3
  Push $R4
  Push $R5
  ${WordFind} "$R0" "|" "+1" $R1
  ${If} $R1 != "UVD1"
    StrCpy $R0 "未收到详细诊断，请保留此错误提示。"
  ${Else}
    ${WordFind} "$R0" "|" "+2" $R1
    ${WordFind} "$R0" "|" "+3" $R3
    ${WordFind} "$R0" "|" "+4" $R4
    ${WordFind} "$R0" "|" "+5" $R5
    StrCpy $R2 "未知步骤（编号 $R1）"
    ${Switch} $R1
      ${Case} 0
        StrCpy $R2 "未提供步骤"
        ${Break}
      ${Case} 1
        StrCpy $R2 "初始化安装辅助程序"
        ${Break}
      ${Case} 2
        StrCpy $R2 "获取系统驱动路径"
        ${Break}
      ${Case} 3
        StrCpy $R2 "打开系统服务管理器"
        ${Break}
      ${Case} 4
        StrCpy $R2 "打开 VeraCrypt 驱动服务"
        ${Break}
      ${Case} 5
        StrCpy $R2 "检查残留驱动文件"
        ${Break}
      ${Case} 6
        StrCpy $R2 "检查驱动设备"
        ${Break}
      ${Case} 7
        StrCpy $R2 "检查驱动服务配置"
        ${Break}
      ${Case} 8
        StrCpy $R2 "验证现有驱动文件"
        ${Break}
      ${Case} 9
        StrCpy $R2 "查询驱动服务状态"
        ${Break}
      ${Case} 10
        StrCpy $R2 "查询驱动接口版本"
        ${Break}
      ${Case} 11
        StrCpy $R2 "检查管理员权限"
        ${Break}
      ${Case} 12
        StrCpy $R2 "验证安装包内驱动文件"
        ${Break}
      ${Case} 13
        StrCpy $R2 "检查系统驱动目录"
        ${Break}
      ${Case} 14
        StrCpy $R2 "设置驱动文件安全权限"
        ${Break}
      ${Case} 15
        StrCpy $R2 "创建系统驱动文件"
        ${Break}
      ${Case} 16
        StrCpy $R2 "打开安装包内驱动文件"
        ${Break}
      ${Case} 17
        StrCpy $R2 "读取驱动文件"
        ${Break}
      ${Case} 18
        StrCpy $R2 "写入系统驱动文件"
        ${Break}
      ${Case} 19
        StrCpy $R2 "刷新系统驱动文件"
        ${Break}
      ${Case} 20
        StrCpy $R2 "验证复制后的系统驱动文件"
        ${Break}
      ${Case} 21
        StrCpy $R2 "创建 VeraCrypt 驱动服务"
        ${Break}
      ${Case} 22
        StrCpy $R2 "启动 VeraCrypt 驱动服务"
        ${Break}
      ${Case} 23
        StrCpy $R2 "安装后重新检测驱动"
        ${Break}
      ${Case} 24
        StrCpy $R2 "请求管理员授权并启动辅助程序"
        ${Break}
      ${Case} 25
        StrCpy $R2 "等待管理员辅助程序结果"
        ${Break}
      ${Case} 26
        StrCpy $R2 "建立驱动诊断通道"
        ${Break}
    ${EndSwitch}
    ${Switch} $R3
      ${Case} 1
        StrCpy $R3 "Win32 $R4"
        ${Break}
      ${Case} 2
        StrCpy $R3 "HRESULT $R4"
        ${Break}
      ${Case} 3
        StrCpy $R3 "辅助进程退出码 $R4（未收到原始错误）"
        ${Break}
      ${Default}
        StrCpy $R3 "未提供原始错误码"
    ${EndSwitch}
    StrCpy $R0 "步骤：$R2$\r$\n底层错误：$R3"
    ${If} $R5 != 0
      StrCpy $R0 "$R0$\r$\n子步骤／状态码：$R5"
    ${EndIf}
  ${EndIf}
  Pop $R5
  Pop $R4
  Pop $R3
  Pop $R2
  Pop $R1
  Exch $R0
FunctionEnd
!endif
