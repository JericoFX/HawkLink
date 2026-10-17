@echo off
setlocal
cd /d F:\HawkenServer
set "OUT=F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\owning-pawn-role\green\luna"
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 > "%OUT%\devshell.stdout.log" 2> "%OUT%\devshell.stderr.log"
set "VS_RC=%ERRORLEVEL%"
> "%OUT%\devshell.exit.txt" echo %VS_RC%
if not "%VS_RC%"=="0" exit /b %VS_RC%
set MSBUILDDISABLENODEREUSE=1
cmake --build --preset windows-msvc --config Release --parallel 1 --target hawken-local-udp-tests hawken-udp-trial-runtime-tests hawken-wreckage-trial-tests hawken-server-listing-tests hawken-wreckage-trial-probe hawken-server > "%OUT%\build.stdout.log" 2> "%OUT%\build.stderr.log"
set "BUILD_RC=%ERRORLEVEL%"
> "%OUT%\build.exit.txt" echo %BUILD_RC%
if not "%BUILD_RC%"=="0" exit /b %BUILD_RC%
"F:\HawkenServer\build\windows-msvc\hawken-local-udp-tests.exe" --test-case="UDP owning Pawn role*" > "%OUT%\owner-focused.stdout.log" 2> "%OUT%\owner-focused.stderr.log"
set "OWNER_FOCUSED_RC=%ERRORLEVEL%"
> "%OUT%\owner-focused.exit.txt" echo %OWNER_FOCUSED_RC%
if not "%OWNER_FOCUSED_RC%"=="0" exit /b %OWNER_FOCUSED_RC%
ctest --preset windows-msvc -C Release --parallel 1 --verbose --output-junit "%OUT%\ctest-results.xml" > "%OUT%\ctest.stdout.log" 2> "%OUT%\ctest.stderr.log"
set "CTEST_RC=%ERRORLEVEL%"
> "%OUT%\ctest.exit.txt" echo %CTEST_RC%
if not "%CTEST_RC%"=="0" exit /b %CTEST_RC%
"F:\HawkenServer\build\windows-msvc\hawken-wreckage-trial-probe.exe" "F:\SteamLibrary\steamapps\common\Hawken" > "%OUT%\wreckage-probe.stdout.log" 2> "%OUT%\wreckage-probe.stderr.log"
set "WRECKAGE_PROBE_RC=%ERRORLEVEL%"
> "%OUT%\wreckage-probe.exit.txt" echo %WRECKAGE_PROBE_RC%
"F:\HawkenServer\build\windows-msvc\hawken-server.exe" --help > "%OUT%\server-help.stdout.log" 2> "%OUT%\server-help.stderr.log"
set "SERVER_HELP_RC=%ERRORLEVEL%"
> "%OUT%\server-help.exit.txt" echo %SERVER_HELP_RC%
set "FINAL_RC=0"
if not "%WRECKAGE_PROBE_RC%"=="0" set "FINAL_RC=%WRECKAGE_PROBE_RC%"
if not "%SERVER_HELP_RC%"=="0" if "%FINAL_RC%"=="0" set "FINAL_RC=%SERVER_HELP_RC%"
> "%OUT%\wrapper.exit.txt" echo %FINAL_RC%
exit /b %FINAL_RC%
