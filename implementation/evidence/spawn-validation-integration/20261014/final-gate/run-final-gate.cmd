@echo off
setlocal
cd /d F:\HawkenServer
set "ROOT=F:\HawkenServer"
set "OUT=F:\HawkenServer\implementation\evidence\spawn-validation-integration\20261014\final-gate\luna"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 > "%OUT%\devshell.stdout.log" 2> "%OUT%\devshell.stderr.log"
set "VS_RC=%ERRORLEVEL%"
> "%OUT%\devshell.exit.txt" echo %VS_RC%
if not "%VS_RC%"=="0" exit /b %VS_RC%
set MSBUILDDISABLENODEREUSE=1
cmake --build --preset windows-msvc --config Release --parallel 1 > "%OUT%\build.stdout.log" 2> "%OUT%\build.stderr.log"
set "BUILD_RC=%ERRORLEVEL%"
> "%OUT%\build.exit.txt" echo %BUILD_RC%
if not "%BUILD_RC%"=="0" exit /b %BUILD_RC%
ctest --test-dir "%ROOT%\build\windows-msvc" --parallel 1 --output-on-failure --no-tests=error --output-junit "%OUT%\tests.xml" > "%OUT%\ctest.stdout.log" 2> "%OUT%\ctest.stderr.log"
set "TEST_RC=%ERRORLEVEL%"
> "%OUT%\ctest.exit.txt" echo %TEST_RC%
if not "%TEST_RC%"=="0" exit /b %TEST_RC%
"%ROOT%\build\windows-msvc\hawken-server.exe" --help > "%OUT%\server-help.stdout.log" 2> "%OUT%\server-help.stderr.log"
set "SERVER_HELP_RC=%ERRORLEVEL%"
> "%OUT%\server-help.exit.txt" echo %SERVER_HELP_RC%
if not "%SERVER_HELP_RC%"=="0" exit /b %SERVER_HELP_RC%
"%ROOT%\build\windows-msvc\hawken-master.exe" --help > "%OUT%\master-help.stdout.log" 2> "%OUT%\master-help.stderr.log"
set "MASTER_HELP_RC=%ERRORLEVEL%"
> "%OUT%\master-help.exit.txt" echo %MASTER_HELP_RC%
> "%OUT%\wrapper.exit.txt" echo %MASTER_HELP_RC%
exit /b %MASTER_HELP_RC%
