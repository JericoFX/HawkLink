@echo off
set MSBUILDDISABLENODEREUSE=1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-rpc-arguments\green\environment.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
cd /d "F:\HawkenServer"
cmake --build build/windows-msvc --config Release --target hawken-weapon-fire-rpc-tests --parallel 1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-rpc-arguments\green\build.log" 2>&1
set "buildExit=%errorlevel%"
echo %buildExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-rpc-arguments\green\build.exit"
if not "%buildExit%"=="0" exit /b %buildExit%
"F:\HawkenServer\build\windows-msvc\hawken-weapon-fire-rpc-tests.exe" --no-colors=1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-rpc-arguments\green\focused.log" 2>&1
set "focusedExit=%errorlevel%"
echo %focusedExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-rpc-arguments\green\focused.exit"
if not "%focusedExit%"=="0" exit /b %focusedExit%
ctest --test-dir build/windows-msvc -C Release --output-on-failure --parallel 1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-rpc-arguments\green\ctest.log" 2>&1
set "testExit=%errorlevel%"
echo %testExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-rpc-arguments\green\ctest.exit"
exit /b %testExit%
