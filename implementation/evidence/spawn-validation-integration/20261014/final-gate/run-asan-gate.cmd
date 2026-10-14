@echo off
setlocal
cd /d F:\HawkenServer
set "ROOT=F:\HawkenServer"
set "OUT=F:\HawkenServer\implementation\evidence\spawn-validation-integration\20261014\final-gate\asan-luna"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 > "%OUT%\devshell.stdout.log" 2> "%OUT%\devshell.stderr.log"
set "VS_RC=%ERRORLEVEL%"
> "%OUT%\devshell.exit.txt" echo %VS_RC%
if not "%VS_RC%"=="0" exit /b %VS_RC%
set MSBUILDDISABLENODEREUSE=1
cmake --build --preset windows-clang-asan --config RelWithDebInfo --parallel 1 --target hawken-local-udp-tests hawken-udp-trial-runtime-tests hawken-storage-tests hawken-spawn-validation-rpc-tests hawken-controller-admission-rpc-tests > "%OUT%\build.stdout.log" 2> "%OUT%\build.stderr.log"
set "BUILD_RC=%ERRORLEVEL%"
> "%OUT%\build.exit.txt" echo %BUILD_RC%
if not "%BUILD_RC%"=="0" exit /b %BUILD_RC%
ctest --preset windows-clang-asan --parallel 1 -R "^(local_udp_owner|udp_trial_runtime|storage|spawn_validation_rpc|controller_admission_rpc)$" --output-on-failure --no-tests=error --output-junit "%OUT%\tests.xml" > "%OUT%\ctest.stdout.log" 2> "%OUT%\ctest.stderr.log"
set "TEST_RC=%ERRORLEVEL%"
> "%OUT%\ctest.exit.txt" echo %TEST_RC%
> "%OUT%\wrapper.exit.txt" echo %TEST_RC%
exit /b %TEST_RC%
