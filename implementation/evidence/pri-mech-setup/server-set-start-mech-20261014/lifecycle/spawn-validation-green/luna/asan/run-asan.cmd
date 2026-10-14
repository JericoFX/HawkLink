@echo off
setlocal
cd /d F:\HawkenServer
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 > "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\devshell.stdout.txt" 2> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\devshell.stderr.txt"
set "rc=%ERRORLEVEL%"
> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\devshell.exit.txt" echo %rc%
if not "%rc%"=="0" exit /b %rc%
set MSBUILDDISABLENODEREUSE=1
where cl > "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\where-cl.stdout.txt" 2> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\where-cl.stderr.txt"
set "rc=%ERRORLEVEL%"
> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\where-cl.exit.txt" echo %rc%
if not "%rc%"=="0" exit /b %rc%
cmake --build --preset windows-msvc-asan --config RelWithDebInfo --parallel 1 --target hawken-spawn-validation-tests > "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\build.stdout.txt" 2> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\build.stderr.txt"
set "rc=%ERRORLEVEL%"
> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\build.exit.txt" echo %rc%
if not "%rc%"=="0" exit /b %rc%
ctest --preset windows-msvc-asan -C RelWithDebInfo -R "^spawn_validation$" -V > "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\ctest.stdout.txt" 2> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\ctest.stderr.txt"
set "rc=%ERRORLEVEL%"
> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\asan\ctest.exit.txt" echo %rc%
exit /b %rc%
