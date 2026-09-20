!ifndef USB_VAULT_UPDATE_FILES
!define USB_VAULT_UPDATE_FILES
!include LogicLib.nsh

; File transaction primitives, not yet enabled by the product installer.
; The caller must provide validated, private staging/backup directories on the
; installation volume, validate all parent directories against reparse points,
; stop the application, and serialize installation/uninstallation before use.
; Paths and IDs are compile-time package entries, never a runtime file manifest.
; Each declared ID is single-use within one installer process. Flags start empty.
; Call Replace in order; on failure call Rollback in reverse order. Commit only
; after ALL files and registration have succeeded. Never remove backup roots
; recursively. These primitives handle reported errors, not process/power loss.
Var UpdateFailed
Var UpdateRollbackFailed
Var UpdateCleanupFailed

!macro UpdateDeclare Id
  Var UpdateOld${Id}
  Var UpdateNew${Id}
!macroend

!macro UpdateReplace Id Source Target Backup
  ${If} $UpdateFailed != 1
    ; Reject directories and reparse-point leaf entries, including staged files.
    System::Call 'kernel32::GetFileAttributesW(w "${Source}") i .r0'
    IntOp $1 $0 & 0x410
    ${If} $0 == -1
    ${OrIf} $1 != 0
      StrCpy $UpdateFailed 1
    ${Else}
      System::Call 'kernel32::GetFileAttributesW(w "${Target}") i .r0 ?e'
      Pop $2
      ${If} $0 == -1
        ${If} $2 != 2
        ${AndIf} $2 != 3
          StrCpy $UpdateFailed 1
        ${EndIf}
      ${Else}
        IntOp $1 $0 & 0x410
        ${If} $1 != 0
          StrCpy $UpdateFailed 1
        ${Else}
          ; MoveFile refuses an existing backup. Do not overwrite recovery data.
          System::Call 'kernel32::MoveFileW(w "${Target}", w "${Backup}") i .r0'
          ${If} $0 == 0
            StrCpy $UpdateFailed 1
          ${Else}
            StrCpy $UpdateOld${Id} 1
            !ifdef UpdateTestCheckpoint
              !insertmacro ${UpdateTestCheckpoint} old-moved
            !endif
          ${EndIf}
        ${EndIf}
      ${EndIf}
      ${If} $UpdateFailed != 1
        System::Call 'kernel32::MoveFileW(w "${Source}", w "${Target}") i .r0'
        ${If} $0 == 0
          StrCpy $UpdateFailed 1
        ${Else}
          StrCpy $UpdateNew${Id} 1
          !ifdef UpdateTestCheckpoint
            !insertmacro ${UpdateTestCheckpoint} new-moved
          !endif
        ${EndIf}
      ${EndIf}
    ${EndIf}
  ${EndIf}
!macroend

!macro UpdateRollback Id Target Backup
  ${If} $UpdateNew${Id} == 1
    System::Call 'kernel32::DeleteFileW(w "${Target}") i .r0'
    ${If} $0 == 0
      StrCpy $UpdateRollbackFailed 1
    ${Else}
      StrCpy $UpdateNew${Id} 0
    ${EndIf}
  ${EndIf}
  ${If} $UpdateOld${Id} == 1
    ; Never overwrite an occupied target, even if rollback was requested.
    System::Call 'kernel32::MoveFileW(w "${Backup}", w "${Target}") i .r0'
    ${If} $0 == 0
      StrCpy $UpdateRollbackFailed 1
    ${Else}
      StrCpy $UpdateOld${Id} 0
    ${EndIf}
  ${EndIf}
!macroend

!macro UpdateCommit Id Backup
  ${If} $UpdateFailed != 1
  ${AndIf} $UpdateRollbackFailed != 1
  ${AndIf} $UpdateOld${Id} == 1
    System::Call 'kernel32::DeleteFileW(w "${Backup}") i .r0'
    ${If} $0 == 0
      StrCpy $UpdateCleanupFailed 1
    ${Else}
      StrCpy $UpdateOld${Id} 0
    ${EndIf}
  ${EndIf}
!macroend
!endif
