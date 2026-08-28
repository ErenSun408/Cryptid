@echo off
setlocal

if "%~1"=="" (
  echo Usage: tools\configure-preset.cmd ^<preset^> [additional CMake arguments] 1>&2
  exit /b 2
)

chcp 65001 >nul
if errorlevel 1 (
  echo Failed to switch the console to UTF-8 before configuring CMake. 1>&2
  exit /b 1
)

cmake --preset %*
exit /b %errorlevel%
