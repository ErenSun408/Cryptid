Unicode true
!pragma warning disable 6010 ; Individual fixtures intentionally omit lock use.
!include "${__FILEDIR__}\..\packaging\windows\update-paths.nsh"
Name "Update path guard test"
OutFile "${TestOutput}"
RequestExecutionLevel user
SilentInstall silent
Section
  StrCpy $0 "${TestPath}"
  Call UpdateGuardDirectory
  StrCpy $R0 $UpdatePathFailed
  !ifdef TestRename
    ${If} $R0 == 0
      System::Call 'kernel32::MoveFileW(w "${TestPath}", w "${TestPath}-moved") i .r0'
      ${If} $0 != 0
        StrCpy $R0 2
      ${EndIf}
    ${EndIf}
  !endif
  !ifdef TestLock
    ${If} $R0 == 0
      StrCpy $0 "${TestPath}\update.lock"
      Call UpdateAcquireLock
      StrCpy $R0 $UpdatePathFailed
      ${If} $R0 == 0
        System::Call 'kernel32::CreateFileW(w "${TestPath}\update.lock", i 0x80000000, i 3, p 0, i 3, i 0x80, p 0) p .r1'
        ${If} $1 != -1
          System::Call 'kernel32::CloseHandle(p r1)'
          StrCpy $R0 2
        ${EndIf}
      ${EndIf}
    ${EndIf}
  !endif
  Call UpdateReleasePaths
  !ifdef TestRename
    ${If} $R0 == 0
      System::Call 'kernel32::MoveFileW(w "${TestPath}", w "${TestPath}-moved") i .r0'
      ${If} $0 == 0
        StrCpy $R0 3
      ${Else}
        System::Call 'kernel32::MoveFileW(w "${TestPath}-moved", w "${TestPath}") i .r0'
        ${If} $0 == 0
          StrCpy $R0 3
        ${EndIf}
      ${EndIf}
    ${EndIf}
  !endif
  SetErrorLevel $R0
SectionEnd
