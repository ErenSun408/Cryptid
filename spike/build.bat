@echo off
REM ============================================================
REM  vcspike 构建脚本
REM  优先用 MSVC（64 位，与驱动架构一致），没有就退回 MinGW
REM ============================================================
setlocal

where cl.exe >nul 2>&1
if %ERRORLEVEL%==0 (
    echo [*] 使用 MSVC 构建 64 位版本...
    cl /nologo /W4 /O2 /Fe:vcspike.exe vcspike.c user32.lib
    goto :done
)

if exist "C:\MinGW\bin\gcc.exe" (
    echo [*] 未找到 MSVC，改用 MinGW 构建 32 位版本...
    echo     注意: 结构体为 pack(1)，32/64 位布局一致，可与 64 位驱动通信。
    "C:\MinGW\bin\gcc.exe" -Wall -O2 -D_WIN32_WINNT=0x0601 -o vcspike.exe vcspike.c -luser32
    goto :done
)

echo [X] 找不到任何可用编译器（cl.exe 或 C:\MinGW\bin\gcc.exe）
exit /b 1

:done
if %ERRORLEVEL%==0 (
    echo.
    echo [OK] 构建成功 -^> vcspike.exe
    echo      编译期断言已通过，结构体布局与驱动 ABI 一致。
    echo.
    echo      下一步: 用管理员身份打开终端，运行 vcspike.exe version
) else (
    echo [X] 构建失败
)
endlocal
