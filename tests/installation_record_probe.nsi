Unicode true
!include LogicLib.nsh
!include "${__FILEDIR__}\..\packaging\windows\installation-record.nsh"
Name "Installation record probe test"
OutFile "${TestOutput}"
RequestExecutionLevel user
SilentInstall silent
Section
  StrCpy $0 "${TestPath}"
  !insertmacro ProbeRecordedInstallation
  SetErrorLevel $1
SectionEnd
