@echo off
REM Spike 3b milestone 2 - acceptance criteria 2 and 3
REM Mount the produced container with both passwords, check reported type and size.
REM ASCII only - cmd.exe reads .bat in the ANSI codepage.
setlocal
cd /d "D:\Desktop\usb-vault\spike"
set VC=D:\Desktop\usb-vault\spike\vcspike.exe
set HC=D:\Desktop\usb-vault\spike\spike3b.hc
REM NOTE: vcspike.c still hardcodes pkcs5_prf=0 (auto-detect), so hidden-volume
REM mounts here take ~6.6s. Fine for correctness; see Spike 6 for the perf rule.

echo ==================================================
echo [A] mount with HIDDEN password  (expect type=1)
echo ==================================================
set VCSPIKE_PASSWORD=HiddenPass123456
"%VC%" mount "%HC%" Y
"%VC%" list
set VCSPIKE_PASSWORD=x
"%VC%" unmount Y -f

echo.
echo ==================================================
echo [B] mount with OUTER password, no protection  (expect type=0, mode B)
echo ==================================================
set VCSPIKE_PASSWORD=OuterPass123456
"%VC%" mount "%HC%" Y
"%VC%" list
set VCSPIKE_PASSWORD=x
"%VC%" unmount Y -f

echo.
echo ==================================================
echo [C] mount-outer WITH protection  (expect type=2, mode C)
echo ==================================================
set VCSPIKE_PASSWORD=OuterPass123456
set VCSPIKE_HIDDEN_PASSWORD=HiddenPass123456
"%VC%" mount-outer "%HC%" Y
"%VC%" list
set VCSPIKE_PASSWORD=x
"%VC%" unmount Y -f

echo.
echo [done]
endlocal
