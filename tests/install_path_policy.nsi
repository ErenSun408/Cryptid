Unicode true
!include LogicLib.nsh
!include "${__FILEDIR__}\..\packaging\windows\install-path-policy.nsh"
Name "Install path policy test"
OutFile "${TestOutput}"
RequestExecutionLevel user
SilentInstall silent
Section
  FileOpen $0 "${TestInput}" r
  FileReadUTF16LE $0 $1
  FileClose $0
  !insertmacro InstallPathAsciiOnly $1 $2
  IntOp $2 $2 + 10
  SetErrorLevel $2
SectionEnd
