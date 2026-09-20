!ifndef USB_VAULT_UPDATE_JOURNAL
!define USB_VAULT_UPDATE_JOURNAL
!include "${__FILEDIR__}\update-files.nsh"

; Recovery is restricted to the SAME compiled package file list. A future
; integration must bind the private journal directory to that package identity,
; hold update-paths guards and the transaction lock, and check the app is closed.
; No paths or commands are read from journal files. All markers are four bytes:
; 827741440 = originally absent, 827741441 = originally present,
; 827741442 = whole transaction committed. Flush intent before changing files;
; flush the global commit marker before cleaning ANY backups.
; Same-volume rename states allow restart after each operation. Unknown states
; fail closed and retain artifacts. Sudden storage loss is not assumed atomic.
; $R0..$R3 and $0..$5 are scratch registers; marker outputs must use $R registers.
Var UpdateRecoveryFailed

; Output: 0 absent, 1 regular file, -1 invalid/unknown; clobbers $0..$2.
!macro UpdateInspectFile Path Output
  StrCpy ${Output} -1
  System::Call 'kernel32::GetFileAttributesW(w "${Path}") i .r0 ?e'
  Pop $2
  ${If} $0 == -1
    ${If} $2 == 2
    ${OrIf} $2 == 3
      StrCpy ${Output} 0
    ${EndIf}
  ${Else}
    IntOp $1 $0 & 0x410
    ${If} $1 == 0
      StrCpy ${Output} 1
    ${EndIf}
  ${EndIf}
!macroend

; CREATE_NEW never truncates old recovery information. On write/flush failure,
; retain the partial marker and stop; do not mutate the target files.
!macro UpdateWriteMarker Path Value
  ${If} $UpdateFailed != 1
    System::Call 'kernel32::CreateFileW(w "${Path}", i 0x40000000, i 0, p 0, i 1, i 0x80000080, p 0) p .r3'
    ${If} $3 == -1
      StrCpy $UpdateFailed 1
    ${Else}
      System::Call 'kernel32::WriteFile(p r3, *i ${Value}, i 4, *i .r4, p 0) i .r5'
      ${If} $5 == 0
      ${OrIf} $4 != 4
        StrCpy $UpdateFailed 1
      ${Else}
        System::Call 'kernel32::FlushFileBuffers(p r3) i .r5'
        ${If} $5 == 0
          StrCpy $UpdateFailed 1
        ${EndIf}
      ${EndIf}
      System::Call 'kernel32::CloseHandle(p r3)'
    ${EndIf}
  ${EndIf}
!macroend

; Output: -2 absent, -1 invalid, otherwise one of the three exact marker values.
; Read through an OPEN_REPARSE_POINT handle and validate the opened object.
!macro UpdateReadMarker Path Output
  StrCpy ${Output} -1
  System::Call 'kernel32::CreateFileW(w "${Path}", i 0x80000000, i 1, p 0, i 3, i 0x00200000, p 0) p .r3 ?e'
  Pop $2
  ${If} $3 == -1
    ${If} $2 == 2
    ${OrIf} $2 == 3
      StrCpy ${Output} -2
    ${EndIf}
  ${Else}
    System::Alloc 52
    Pop $0
    ${If} $0 != 0
      System::Call 'kernel32::GetFileInformationByHandle(p r3, p r0) i .r5'
      System::Call '*$0(i .r1)'
      System::Free $0
      IntOp $1 $1 & 0x410
      ${If} $5 != 0
      ${AndIf} $1 == 0
        System::Call 'kernel32::GetFileSize(p r3, *i .r2) i .r4'
        ${If} $4 == 4
        ${AndIf} $2 == 0
          System::Call 'kernel32::ReadFile(p r3, *i .r1, i 4, *i .r4, p 0) i .r5'
          ${If} $5 != 0
          ${AndIf} $4 == 4
            ${If} $1 == 827741440
            ${OrIf} $1 == 827741441
            ${OrIf} $1 == 827741442
              StrCpy ${Output} $1
            ${EndIf}
          ${EndIf}
        ${EndIf}
      ${EndIf}
    ${EndIf}
    System::Call 'kernel32::CloseHandle(p r3)'
  ${EndIf}
!macroend

!macro UpdateLoadCommit Path Output
  !insertmacro UpdateReadMarker "${Path}" ${Output}
  ${If} ${Output} == -2
    StrCpy ${Output} 0
  ${ElseIf} ${Output} == 827741442
    StrCpy ${Output} 1
  ${Else}
    StrCpy $UpdateRecoveryFailed 1
  ${EndIf}
!macroend

!macro UpdateJournaledReplace Id Source Target Backup
  ${If} $UpdateFailed != 1
    !insertmacro UpdateInspectFile "${Source}" $R0
    !insertmacro UpdateInspectFile "${Target}" $R1
    !insertmacro UpdateInspectFile "${Backup}" $R2
    ${If} $R0 != 1
    ${OrIf} $R1 == -1
    ${OrIf} $R2 != 0
      StrCpy $UpdateFailed 1
    ${Else}
      IntOp $R1 $R1 + 827741440
      !insertmacro UpdateWriteMarker "${Backup}.intent" $R1
      !ifdef UpdateTestCheckpoint
        !insertmacro ${UpdateTestCheckpoint} intent-flushed
      !endif
      !insertmacro UpdateReplace ${Id} "${Source}" "${Target}" "${Backup}"
    ${EndIf}
  ${EndIf}
!macroend

; Call in reverse package order after reading the global commit marker. Pass
; Committed=0 only for a missing global marker, 1 only for validated 827741442.
; A corrupt global marker MUST stop recovery before invoking any file macro.
; Recovery moves replacement files back to staging; it never deletes an
; ambiguous target. Completed recovery may be safely run again.
!macro UpdateRecoverFile Source Target Backup Committed
  ${If} ${Committed} != 0
  ${AndIf} ${Committed} != 1
    StrCpy $UpdateRecoveryFailed 1
  ${EndIf}
  ${If} $UpdateRecoveryFailed != 1
    !insertmacro UpdateReadMarker "${Backup}.intent" $R3
    !insertmacro UpdateInspectFile "${Source}" $R0
    !insertmacro UpdateInspectFile "${Target}" $R1
    !insertmacro UpdateInspectFile "${Backup}" $R2
    ${If} $R0 == -1
    ${OrIf} $R1 == -1
    ${OrIf} $R2 == -1
      StrCpy $UpdateRecoveryFailed 1
    ${ElseIf} $R3 == -2
      ; Intent is flushed before rename. An unaccounted backup is unsafe.
      ${If} $R2 != 0
        StrCpy $UpdateRecoveryFailed 1
      ${EndIf}
    ${ElseIf} $R3 != 827741440
    ${AndIf} $R3 != 827741441
      StrCpy $UpdateRecoveryFailed 1
    ${ElseIf} ${Committed} == 1
      ${If} $R1 != 1
      ${OrIf} $R0 != 0
        StrCpy $UpdateRecoveryFailed 1
      ${ElseIf} $R2 == 1
        ${If} $R3 != 827741441
          StrCpy $UpdateRecoveryFailed 1
        ${Else}
          System::Call 'kernel32::DeleteFileW(w "${Backup}") i .r0'
          ${If} $0 == 0
            StrCpy $UpdateRecoveryFailed 1
          ${EndIf}
        ${EndIf}
      ${EndIf}
    ${ElseIf} $R3 == 827741441
      ${If} $R2 == 1
        ${If} $R0 == 0
        ${AndIf} $R1 == 1
          System::Call 'kernel32::MoveFileW(w "${Target}", w "${Source}") i .r0'
          ${If} $0 == 0
            StrCpy $UpdateRecoveryFailed 1
          ${Else}
            StrCpy $R0 1
            StrCpy $R1 0
            !ifdef UpdateTestCheckpoint
              !insertmacro ${UpdateTestCheckpoint} new-restaged
            !endif
          ${EndIf}
        ${EndIf}
        ${If} $R0 != 1
        ${OrIf} $R1 != 0
          StrCpy $UpdateRecoveryFailed 1
        ${ElseIf} $UpdateRecoveryFailed != 1
          System::Call 'kernel32::MoveFileW(w "${Backup}", w "${Target}") i .r0'
          ${If} $0 == 0
            StrCpy $UpdateRecoveryFailed 1
          ${Else}
            !ifdef UpdateTestCheckpoint
              !insertmacro ${UpdateTestCheckpoint} old-restored
            !endif
          ${EndIf}
        ${EndIf}
      ${ElseIf} $R0 != 1
      ${OrIf} $R1 != 1
        StrCpy $UpdateRecoveryFailed 1
      ${EndIf}
    ${Else}
      ; Originally absent: there must never be an old backup.
      ${If} $R2 != 0
        StrCpy $UpdateRecoveryFailed 1
      ${ElseIf} $R0 == 0
      ${AndIf} $R1 == 1
        System::Call 'kernel32::MoveFileW(w "${Target}", w "${Source}") i .r0'
        ${If} $0 == 0
          StrCpy $UpdateRecoveryFailed 1
        ${EndIf}
      ${ElseIf} $R0 != 1
      ${OrIf} $R1 != 0
        StrCpy $UpdateRecoveryFailed 1
      ${EndIf}
    ${EndIf}
    ${If} $UpdateRecoveryFailed != 1
    ${AndIf} $R3 != -2
      System::Call 'kernel32::DeleteFileW(w "${Backup}.intent") i .r0'
      ${If} $0 == 0
        StrCpy $UpdateRecoveryFailed 1
      ${EndIf}
    ${EndIf}
  ${EndIf}
!macroend
!endif
