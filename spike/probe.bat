@echo off
REM Build and run probe_bitmap against the Spike 3b outer volume (FAT).
REM ASCII only - cmd.exe reads .bat in the ANSI codepage.
setlocal
set PATH=%PATH%;D:\BuildTools\bin
call "D:\BuildTools\VS2022\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "D:\Desktop\usb-vault\spike"
set VC=D:\Desktop\usb-vault\spike\vcspike.exe
set HC=D:\Desktop\usb-vault\spike\spike3b.hc

cl /nologo /W3 /O2 /source-charset:utf-8 /Fe:probe_bitmap.exe probe_bitmap.c advapi32.lib >nul
if errorlevel 1 ( echo [X] build failed & exit /b 1 )

echo.
echo ### outer volume, FAT ###
set VCSPIKE_PASSWORD=OuterPass123456
"%VC%" mount "%HC%" Y >nul
"D:\Desktop\usb-vault\spike\probe_bitmap.exe" Y
set VCSPIKE_PASSWORD=x
"%VC%" unmount Y -f >nul

echo.
echo ### control: an existing NTFS volume (C:) ###
"D:\Desktop\usb-vault\spike\probe_bitmap.exe" C

endlocal
