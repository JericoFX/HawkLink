@echo off
setlocal
set MSBUILDDISABLENODEREUSE=1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\red\environment.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
cd /d "F:\HawkenServer"
cmake --build build/windows-msvc --target hawken-live-player-reader-tests --parallel 1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\red\build.log" 2>&1
set "buildExit=%errorlevel%"
> "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\red\build.exit" echo %buildExit%
if not "%buildExit%"=="0" exit /b %buildExit%
"F:\HawkenServer\build\windows-msvc\hawken-live-player-reader-tests.exe" --test-case="Live player gravity*" > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\red\focused.log" 2>&1
set "testExit=%errorlevel%"
> "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\live-dispatch-reader\red\focused.exit" echo %testExit%
exit /b %testExit%
