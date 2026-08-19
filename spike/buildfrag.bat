@echo off
REM Build frag_test. ASCII only - cmd.exe reads .bat in the ANSI codepage.
setlocal
set PATH=%PATH%;D:\BuildTools\bin
call "D:\BuildTools\VS2022\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "D:\Desktop\usb-vault\spike"
cl /nologo /W3 /O2 /EHsc /source-charset:utf-8 /I "_vcref\src\FormatDLL" /Fe:frag_test.exe frag_test.cpp /link "_vcref\src\x64\Release\VeraCryptFormat.lib" advapi32.lib
if errorlevel 1 ( echo [X] build failed & exit /b 1 )
echo [OK] built frag_test.exe
endlocal
