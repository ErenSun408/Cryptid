Unicode true
!pragma warning disable 6001
!pragma warning disable 6010
!pragma warning disable 6020
!include FileFunc.nsh
!ifndef StopAt
  !define StopAt "never"
!endif
!define UpdateTestCheckpoint EngineTestCheckpoint
!define InstallerTestCheckpoint EngineTestCheckpoint
!macro EngineTestCheckpoint Position
  !if "${Position}" == "${StopAt}"
    System::Call 'kernel32::TerminateProcess(p -1, i 99)'
  !endif
!macroend
!define UninstallKey "Software\USBVaultInstallerTests\${TestId}"
!define ProductMenu "${TestRoot}\menu"
!define ProductWindowClass "USBVaultInstallerTest_${TestId}"
!define ProductConfigurationDirectory "${TestRoot}\config"
!define ProductCredentialPrefix "usb-vault-installer-test-${TestId}"
!include "${__FILEDIR__}\..\packaging\windows\installer-engine.nsh"
Name "USB Vault isolated installer test"
OutFile "${TestOutput}"
RequestExecutionLevel user
SilentInstall silent
Section
  StrCpy $INSTDIR "${TestRoot}\app"
  ReadRegStr $0 HKCU "${UninstallKey}" "InstallLocation"
  ${If} $0 == $INSTDIR
    StrCpy $ExistingInstall 1
  ${EndIf}
  Call AcquireSetupMutex
  ${If} $UpdateFailed != 1
    Call InstallPackage
  ${EndIf}
  Call ReleaseSetup
  SetErrorLevel 0
  ${If} $UpdateFailed == 1
    SetErrorLevel 10
  ${EndIf}
SectionEnd
Section "Uninstall"
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/DELETECONFIG=" $1
  ${IfNot} ${Errors}
    StrCpy $UninstallDeleteConfiguration $1
  ${EndIf}
  Call un.AcquireSetupMutex
  ${If} $UpdateFailed != 1
    Call un.UninstallPackage
  ${EndIf}
  Call un.ReleaseSetup
  SetErrorLevel 0
  ${If} $UpdateFailed == 1
    SetErrorLevel 10
  ${EndIf}
SectionEnd
