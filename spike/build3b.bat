@echo off
REM ============================================================
REM  Spike 3b milestone 2 build script
REM   1) rebuild patched FormatDLL (hiddenVol passthrough)
REM   2) copy DLL next to the harness
REM   3) compile spike3b_hidden.exe
REM  Ref: SPIKE3B build steps section
REM  NOTE: ASCII only - cmd.exe reads .bat in the ANSI codepage.
REM ============================================================
setlocal

set PATH=%PATH%;D:\BuildTools\bin
call "D:\BuildTools\VS2022\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo [X] vcvars64 failed & exit /b 1 )

pushd "D:\Desktop\usb-vault\spike\_vcref\src"

echo [1/3] building FormatDLL ...
msbuild VeraCrypt.sln /t:FormatDLL /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
if errorlevel 1 ( echo [X] FormatDLL build failed & popd & exit /b 1 )

popd

echo [2/3] copying DLL ...
copy /Y "D:\Desktop\usb-vault\spike\_vcref\src\x64\Release\VeraCryptFormat.dll" "D:\Desktop\usb-vault\spike\VeraCryptFormat.dll" >nul
if errorlevel 1 ( echo [X] copy failed & exit /b 1 )

echo [3/3] compiling harness ...
pushd "D:\Desktop\usb-vault\spike"
REM /source-charset:utf-8 - the .cpp is UTF-8; execution charset stays ANSI(936) so
REM console output renders correctly in a GBK console.
cl /nologo /W3 /O2 /EHsc /source-charset:utf-8 /I "_vcref\src\FormatDLL" /Fe:spike3b_hidden.exe spike3b_hidden.cpp /link "_vcref\src\x64\Release\VeraCryptFormat.lib" advapi32.lib
if errorlevel 1 ( echo [X] harness build failed & popd & exit /b 1 )
popd

echo.
echo [OK] build complete
endlocal
