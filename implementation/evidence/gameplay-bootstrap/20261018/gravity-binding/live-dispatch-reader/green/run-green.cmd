@echo off
setlocal
set MSBUILDDISABLENODEREUSE=1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\environment.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
cd /d "F:\HawkenServer"
cmake --build build/windows-msvc --target hawken-live-player-reader-tests hawken-controller-netfields-probe --parallel 1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\build.log" 2>&1
set "buildExit=%errorlevel%"
> "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\build.exit" echo %buildExit%
if not "%buildExit%"=="0" exit /b %buildExit%
"F:\HawkenServer\build\windows-msvc\hawken-live-player-reader-tests.exe" > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\reader-full.log" 2>&1
set "readerExit=%errorlevel%"
> "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\reader-full.exit" echo %readerExit%
if not "%readerExit%"=="0" exit /b %readerExit%
"F:\HawkenServer\build\windows-msvc\hawken-controller-netfields-probe.exe" --help > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\help.log" 2>&1
set "helpExit=%errorlevel%"
> "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\help.exit" echo %helpExit%
if not "%helpExit%"=="0" exit /b %helpExit%
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\check-cli.ps1" > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\cli.log" 2>&1
set "cliExit=%errorlevel%"
> "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\green\cli.exit" echo %cliExit%
exit /b %cliExit%
