!ifndef USB_VAULT_UPDATE_PATHS
!define USB_VAULT_UPDATE_PATHS
!include LogicLib.nsh

; Call with $0 = existing absolute DOS directory on a fixed drive.
; Output $UpdatePathFailed; clobbers $0..$9. Handles remain held until release.
; Walk from the drive root down, opening every component without following
; reparse points and without FILE_SHARE_DELETE. This pins the ancestry before
; subsequent child lookups. No registry or runtime manifest paths are executed.
Var UpdatePathFailed
Var UpdateDirectoryHandles
Var UpdateLockHandle

Function UpdateGuardDirectory
  StrCpy $UpdatePathFailed 1
  StrLen $1 $0
  IntCmp $1 3 guard_invalid guard_invalid 0
  IntOp $2 ${NSIS_MAX_STRLEN} - 1
  IntCmp $1 $2 guard_invalid 0 guard_invalid
  ; GetFullPathName is not validation: reject aliases rather than normalizing
  ; relative paths, dot segments, ADS, slash separators, and trailing spaces.
  StrCpy $2 $0 1 1
  StrCmp $2 ":" 0 guard_invalid
  StrCpy $2 $0 1 2
  StrCmp $2 "\" 0 guard_invalid
  StrCpy $9 $0
  StrCpy $4 $9 3
  System::Call 'kernel32::GetDriveTypeW(w r4) i .r1'
  IntCmp $1 3 0 guard_invalid guard_invalid
  StrCpy $5 3
  StrCpy $8 ""
guard_open:
  ; Include FILE_LIST_DIRECTORY: metadata-only opens do not enforce the share
  ; restriction against directory rename on Windows.
  System::Call 'kernel32::CreateFileW(w r4, i 0x81, i 3, p 0, i 3, i 0x02200000, p 0) p .r6'
  StrCmp $6 -1 guard_invalid
  System::Alloc 52
  Pop $7
  StrCmp $7 0 guard_close_invalid
  System::Call 'kernel32::GetFileInformationByHandle(p r6, p r7) i .r1'
  System::Call '*$7(i .r2)'
  System::Free $7
  StrCmp $1 0 guard_close_invalid
  IntOp $2 $2 & 0x410
  IntCmp $2 0x10 0 guard_close_invalid guard_close_invalid
  System::Alloc 8
  Pop $7
  StrCmp $7 0 guard_close_invalid
  System::Call '*$7(p r6, p $UpdateDirectoryHandles)'
  StrCpy $UpdateDirectoryHandles $7
  StrCpy $2 $9 1 $5
  StrCmp $2 "" guard_valid
guard_component:
  StrCpy $2 $9 1 $5
  StrCmp $2 "" guard_component_done
  StrCmp $2 "\" guard_component_done
  StrCmp $2 "/" guard_invalid
  StrCmp $2 ":" guard_invalid
  StrCmp $2 "*" guard_invalid
  StrCmp $2 "?" guard_invalid
  StrCpy $8 "$8$2"
  IntOp $5 $5 + 1
  Goto guard_component
guard_component_done:
  StrCmp $8 "" guard_invalid
  StrCmp $8 "." guard_invalid
  StrCmp $8 ".." guard_invalid
  StrCpy $1 $8 1 -1
  StrCmp $1 "." guard_invalid
  StrCmp $1 " " guard_invalid
  StrLen $1 $4
  ${If} $1 == 3
    StrCpy $4 "$4$8"
  ${Else}
    StrCpy $4 "$4\$8"
  ${EndIf}
  StrCpy $8 ""
  StrCmp $2 "" guard_open
  IntOp $5 $5 + 1
  ; Reject trailing separators rather than treating them as a second spelling.
  StrCpy $2 $9 1 $5
  StrCmp $2 "" guard_invalid
  Goto guard_open
guard_close_invalid:
  System::Call 'kernel32::CloseHandle(p r6)'
guard_invalid:
  Return
guard_valid:
  StrCpy $UpdatePathFailed 0
FunctionEnd

; Input $0 = lock path inside a guarded private transaction directory.
; Do not use an existing entry: stale lock entries are handled by recovery.
; Exclusive handle blocks another updater for this transaction directory.
Function UpdateAcquireLock
  StrCpy $UpdatePathFailed 1
  StrCmp $UpdateLockHandle "" 0 lock_done
  System::Call 'kernel32::CreateFileW(w r0, i 0xC0000000, i 0, p 0, i 1, i 0x04000080, p 0) p .r1'
  StrCmp $1 -1 lock_done
  StrCpy $UpdateLockHandle $1
  StrCpy $UpdatePathFailed 0
lock_done:
FunctionEnd

Function UpdateReleasePaths
  ${If} $UpdateLockHandle != ""
    System::Call 'kernel32::CloseHandle(p $UpdateLockHandle)'
    StrCpy $UpdateLockHandle ""
  ${EndIf}
  ${Do}
    ${If} $UpdateDirectoryHandles == ""
    ${OrIf} $UpdateDirectoryHandles == 0
      ${ExitDo}
    ${EndIf}
    StrCpy $0 $UpdateDirectoryHandles
    System::Call '*$0(p .r1, p .r2)'
    StrCpy $UpdateDirectoryHandles $2
    System::Call 'kernel32::CloseHandle(p r1)'
    System::Free $0
  ${Loop}
FunctionEnd
!endif
