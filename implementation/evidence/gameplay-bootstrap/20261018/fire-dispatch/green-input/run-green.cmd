@echo off
set MSBUILDDISABLENODEREUSE=1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\fire-dispatch\green-input\environment.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
cd /d "F:\HawkenServer"
cmake --build build/windows-msvc --config Release --target hawken-local-udp-tests hawken-udp-trial-runtime-tests hawken-server-listing-tests hawken-wreckage-trial-tests hawken-wreckage-trial-probe CMakeFiles/hawken-server.dir/src/server_main.cpp.obj --parallel 1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\fire-dispatch\green-input\build.log" 2>&1
set "buildExit=%errorlevel%"
echo %buildExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\fire-dispatch\green-input\build.exit"
if not "%buildExit%"=="0" exit /b %buildExit%
"F:\HawkenServer\build\windows-msvc\hawken-local-udp-tests.exe" "--test-case=UDP weapon fire state*" --no-colors=1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\fire-dispatch\green-input\fire.log" 2>&1
set "fireExit=%errorlevel%"
echo %fireExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\fire-dispatch\green-input\fire.exit"
if not "%fireExit%"=="0" exit /b %fireExit%
"F:\HawkenServer\build\windows-msvc\hawken-wreckage-trial-probe.exe" "F:\SteamLibrary\steamapps\common\Hawken" > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\fire-dispatch\green-input\policy.log" 2>&1
set "policyExit=%errorlevel%"
echo %policyExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\fire-dispatch\green-input\policy.exit"
if not "%policyExit%"=="0" exit /b %policyExit%
ctest --test-dir build/windows-msvc -C Release --output-on-failure --parallel 1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\fire-dispatch\green-input\ctest.log" 2>&1
set "testExit=%errorlevel%"
echo %testExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\fire-dispatch\green-input\ctest.exit"
exit /b %testExit%
