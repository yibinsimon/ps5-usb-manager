@echo off
setlocal EnableExtensions

rem ============================================================
rem  usbmanage - open the volume-selection page in your browser
rem
rem  Usage:    open-ui.cmd [PS5_IP] [PORT]
rem  Default:  open-ui.cmd              :  192.168.1.100 : 9100
rem
rem  Why:  a PS5 payload cannot pop up a window by itself.
rem        The UI is a web page served by the payload on port 9100,
rem        so it has to be opened in a browser - this script does that
rem        in one double-click and tells you if the payload is not up.
rem ============================================================

set "IP=%~1"
if "%IP%"=="" set "IP=192.168.1.100"
set "PORT=%~2"
if "%PORT%"=="" set "PORT=9100"
set "URL=http://%IP%:%PORT%/"

echo.
echo   usbmanage UI   %URL%
echo   checking %IP%:%PORT% ...
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('%IP%', %PORT%); $c.Close() } catch { exit 1 }" >nul 2>&1

if errorlevel 1 goto :unreachable

echo   OK - opening in your default browser.
echo.
start "" "%URL%"
exit /b 0

:unreachable
echo   Not reachable.
echo.
echo   Check these:
echo     1. Is the usbmanage payload loaded on the PS5?
echo        DB toolbox - select usbmanage.elf - click Load.
echo     2. Is this PC on the same LAN as the PS5?
echo     3. Wrong address? Run:  open-ui.cmd 192.168.1.50 9100
echo.
echo   You can also just open this manually:  %URL%
echo.
pause
exit /b 1
