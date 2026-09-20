Unicode true
!pragma warning disable 6001 ; Low-level non-journal cleanup flags unused here.
!include FileFunc.nsh
!define UpdateTestCheckpoint RecoveryCheckpoint
!macro RecoveryCheckpoint Position
  !if "${Position}" == "${StopAt}"
    System::Call 'kernel32::TerminateProcess(p -1, i 99)'
  !endif
!macroend
!include "${__FILEDIR__}\..\packaging\windows\update-journal.nsh"
!include "${__FILEDIR__}\..\packaging\windows\update-paths.nsh"
!insertmacro UpdateDeclare A
Name "Update recovery test"
OutFile "${TestOutput}"
RequestExecutionLevel user
SilentInstall silent
Section
  StrCpy $0 "${TestRoot}\app"
  Call UpdateGuardDirectory
  ${If} $UpdatePathFailed == 1
    SetErrorLevel 40
    Quit
  ${EndIf}
  StrCpy $0 "${TestRoot}\stage"
  Call UpdateGuardDirectory
  ${If} $UpdatePathFailed == 1
    SetErrorLevel 40
    Quit
  ${EndIf}
  StrCpy $0 "${TestRoot}\backup"
  Call UpdateGuardDirectory
  ${If} $UpdatePathFailed == 1
    SetErrorLevel 40
    Quit
  ${EndIf}
  StrCpy $0 "${TestRoot}\backup\update.lock"
  Call UpdateAcquireLock
  ${If} $UpdatePathFailed == 1
    SetErrorLevel 40
    Quit
  ${EndIf}
  ${GetParameters} $R8
  ${If} $R8 == "replace"
    !insertmacro UpdateJournaledReplace A "${TestRoot}\stage\a" "${TestRoot}\app\a" "${TestRoot}\backup\a"
    ${If} $UpdateFailed == 1
      SetErrorLevel 10
    ${Else}
      !insertmacro UpdateWriteMarker "${TestRoot}\backup\committed" 827741442
      !insertmacro RecoveryCheckpoint committed
      ${If} $UpdateFailed == 1
        SetErrorLevel 10
      ${EndIf}
    ${EndIf}
  ${Else}
    !insertmacro UpdateLoadCommit "${TestRoot}\backup\committed" $R7
    !insertmacro UpdateRecoverFile "${TestRoot}\stage\a" "${TestRoot}\app\a" "${TestRoot}\backup\a" $R7
    ${If} $UpdateRecoveryFailed == 1
      SetErrorLevel 20
    ${EndIf}
  ${EndIf}
  Call UpdateReleasePaths
SectionEnd
