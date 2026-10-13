Unicode true
Name "USB Vault driver diagnostic tests"
OutFile "${OutputFile}"
RequestExecutionLevel user
SilentInstall silent
!include /CHARSET=UTF8 "${__FILEDIR__}\..\packaging\windows\driver-diagnostics.nsh"
!macro Check Input Expected Failure
  Push '${Input}'
  Call FormatDriverDiagnostic
  Pop $0
  ${If} $0 != '${Expected}'
    SetErrorLevel ${Failure}
    Quit
  ${EndIf}
!macroend
Section
  StrCpy $R0 "preserved-0"
  StrCpy $R1 "preserved-1"
  StrCpy $R2 "preserved-2"
  StrCpy $R3 "preserved-3"
  StrCpy $R4 "preserved-4"
  StrCpy $R5 "preserved-5"
  !insertmacro Check "UVD1|22|1|0x00000241|0" "步骤：启动 VeraCrypt 驱动服务$\r$\n底层错误：Win32 0x00000241" 1
  !insertmacro Check "UVD1|12|2|0x800B0109|34" "步骤：验证安装包内驱动文件$\r$\n底层错误：HRESULT 0x800B0109$\r$\n子步骤／状态码：34" 2
  !insertmacro Check "UVD1|20|0|0x00000000|7" "步骤：验证复制后的系统驱动文件$\r$\n底层错误：未提供原始错误码$\r$\n子步骤／状态码：7" 3
  !insertmacro Check "UVD1|25|3|0x00000016|0" "步骤：等待管理员辅助程序结果$\r$\n底层错误：辅助进程退出码 0x00000016（未收到原始错误）" 4
  !insertmacro Check "" "未收到详细诊断，请保留此错误提示。" 5
  !insertmacro Check "unstructured private error" "未收到详细诊断，请保留此错误提示。" 6
  ${If} $R0 != "preserved-0"
  ${OrIf} $R1 != "preserved-1"
  ${OrIf} $R2 != "preserved-2"
  ${OrIf} $R3 != "preserved-3"
  ${OrIf} $R4 != "preserved-4"
  ${OrIf} $R5 != "preserved-5"
    SetErrorLevel 7
    Quit
  ${EndIf}
!ifdef CheckUnelevatedHelper
  nsExec::ExecToStack '"${SetupHelper}" --install-driver-elevated'
  Pop $0
  Pop $1
  ${If} $0 != 12
    SetErrorLevel 8
    Quit
  ${EndIf}
  Push $1
  Call FormatDriverDiagnostic
  Pop $0
  ${If} $0 != "步骤：检查管理员权限$\r$\n底层错误：Win32 0x000002E4"
    SetErrorLevel 9
    Quit
  ${EndIf}
!endif
  SetErrorLevel 0
SectionEnd
