@echo off
REM Build patched FormatDLL (now with VeraCryptFormat_ChangePassword) + chpwd harness.
REM ASCII only - cmd.exe reads .bat in the ANSI codepage.
setlocal
set PATH=%PATH%;D:\BuildTools\bin
call "D:\BuildTools\VS2022\VC\Auxiliary\Build\vcvars64.bat" >nul

pushd "D:\Desktop\usb-vault\spike\_vcref\src"
echo [1/3] building FormatDLL ...
msbuild VeraCrypt.sln /t:FormatDLL /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
if errorlevel 1 ( echo [X] FormatDLL build failed & popd & exit /b 1 )
popd

echo [2/3] copying DLL ...
copy /Y "D:\Desktop\usb-vault\spike\_vcref\src\x64\Release\VeraCryptFormat.dll" "D:\Desktop\usb-vault\spike\VeraCryptFormat.dll" >nul

echo [3/3] compiling harness ...
pushd "D:\Desktop\usb-vault\spike"
cl /nologo /W3 /O2 /EHsc /source-charset:utf-8 /I "_vcref\src\FormatDLL" /Fe:spike3b_chpwd.exe spike3b_chpwd.cpp /link "_vcref\src\x64\Release\VeraCryptFormat.lib" user32.lib
if errorlevel 1 ( echo [X] harness build failed & popd & exit /b 1 )
popd

echo.
echo [OK] build complete
endlocal
