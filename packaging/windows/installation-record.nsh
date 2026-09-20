!ifndef USB_VAULT_INSTALLATION_RECORD
!define USB_VAULT_INSTALLATION_RECORD

; Input: $0 = recorded installation directory.
; Output: $1 = 0 (absent), 10 (exists), 20 (cannot determine).
; Clobbers $2. Only explicit Windows not-found errors mean absent.
; Do not remove the record: a successful installation replaces it.
!macro ProbeRecordedInstallation
  System::Call 'kernel32::GetFileAttributesW(w r0) i .r1 ?e'
  Pop $2
  ${If} $1 != -1
    StrCpy $1 10
  ${ElseIf} $2 == 2
  ${OrIf} $2 == 3
    StrCpy $1 0
  ${Else}
    StrCpy $1 20
  ${EndIf}
!macroend

!endif
