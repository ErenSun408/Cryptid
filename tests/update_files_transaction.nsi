Unicode true
!include "${__FILEDIR__}\..\packaging\windows\update-files.nsh"
!insertmacro UpdateDeclare A
!insertmacro UpdateDeclare B
Name "Update file transaction test"
OutFile "${TestOutput}"
RequestExecutionLevel user
SilentInstall silent
Section
  !insertmacro UpdateReplace A "${TestRoot}\stage\a" "${TestRoot}\app\a" "${TestRoot}\backup\a"
  !insertmacro UpdateReplace B "${TestRoot}\stage\b" "${TestRoot}\app\b" "${TestRoot}\backup\b"
  !ifdef ForceRollback
    StrCpy $UpdateFailed 1
  !endif
  !ifdef BlockRollback
    System::Call 'kernel32::CreateFileW(w "${TestRoot}\app\a", i 0x80000000, i 0, p 0, i 3, i 0, p 0) p .r9'
  !endif
  !ifdef BlockCleanup
    System::Call 'kernel32::CreateFileW(w "${TestRoot}\backup\a", i 0x80000000, i 0, p 0, i 3, i 0, p 0) p .r9'
  !endif
  ${If} $UpdateFailed == 1
    !insertmacro UpdateRollback B "${TestRoot}\app\b" "${TestRoot}\backup\b"
    !insertmacro UpdateRollback A "${TestRoot}\app\a" "${TestRoot}\backup\a"
    SetErrorLevel 10
    ${If} $UpdateRollbackFailed == 1
      SetErrorLevel 20
    ${EndIf}
  ${Else}
    !insertmacro UpdateCommit A "${TestRoot}\backup\a"
    !insertmacro UpdateCommit B "${TestRoot}\backup\b"
    SetErrorLevel 0
    ${If} $UpdateCleanupFailed == 1
      SetErrorLevel 30
    ${EndIf}
  ${EndIf}
  !ifdef BlockRollback
    System::Call 'kernel32::CloseHandle(p r9)'
  !endif
  !ifdef BlockCleanup
    System::Call 'kernel32::CloseHandle(p r9)'
  !endif
SectionEnd
