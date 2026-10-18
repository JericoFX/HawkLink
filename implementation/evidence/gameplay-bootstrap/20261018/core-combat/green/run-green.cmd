@echo off
set MSBUILDDISABLENODEREUSE=1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\core-combat\green\environment.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
cd /d "F:\HawkenServer"
cmake --build build/windows-msvc --config Release --target hawken-movement-reply-tests hawken-weapon-fire-state-tests --parallel 1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\core-combat\green\build.log" 2>&1
set "buildExit=%errorlevel%"
echo %buildExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\core-combat\green\build.exit"
if not "%buildExit%"=="0" exit /b %buildExit%
"F:\HawkenServer\build\windows-msvc\hawken-movement-reply-tests.exe" --no-colors=1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\core-combat\green\movement.log" 2>&1
set "movementExit=%errorlevel%"
echo %movementExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\core-combat\green\movement.exit"
if not "%movementExit%"=="0" exit /b %movementExit%
"F:\HawkenServer\build\windows-msvc\hawken-weapon-fire-state-tests.exe" --no-colors=1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\core-combat\green\fire.log" 2>&1
set "fireExit=%errorlevel%"
echo %fireExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\core-combat\green\fire.exit"
if not "%fireExit%"=="0" exit /b %fireExit%
ctest --test-dir build/windows-msvc -C Release --output-on-failure --parallel 1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\core-combat\green\ctest.log" 2>&1
set "testExit=%errorlevel%"
echo %testExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\core-combat\green\ctest.exit"
exit /b %testExit%
