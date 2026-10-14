!ifndef USB_VAULT_INSTALL_PATH_POLICY_NSH
!define USB_VAULT_INSTALL_PATH_POLICY_NSH

; An installation path may hold only 7-bit ASCII. A path with Chinese or any
; other non-ASCII character breaks the VeraCrypt driver deployment, so such a
; path is refused rather than repaired.
;
; Result: 1 = ASCII only, 0 = holds a non-ASCII character, -1 = the check
; itself failed (callers treat it as a refusal). Code page 20127 with
; WC_NO_BEST_FIT_CHARS reports every character outside ASCII through the
; used-default flag instead of mapping it to a look-alike.
!macro InstallPathAsciiOnly Path Result
  Push $8
  Push $9
  StrCpy $9 0
  System::Call 'kernel32::WideCharToMultiByte(i 20127, i 0x400, w "${Path}", i -1, p 0, i 0, p 0, *i 0 r9) i .r8'
  ${If} $8 == 0
    StrCpy $8 -1
  ${ElseIf} $9 != 0
    StrCpy $8 0
  ${Else}
    StrCpy $8 1
  ${EndIf}
  Pop $9
  Exch $8
  Pop ${Result}
!macroend

!endif
