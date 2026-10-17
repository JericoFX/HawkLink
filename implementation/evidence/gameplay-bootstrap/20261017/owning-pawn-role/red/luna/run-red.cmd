@echo off
setlocal
cd /d F:\HawkenServer
set "OUT=F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\owning-pawn-role\red\luna"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 > "%OUT%\devshell.stdout.log" 2> "%OUT%\devshell.stderr.log"
set "ENV_RC=%ERRORLEVEL%"
> "%OUT%\devshell.exit.txt" echo %ENV_RC%
if not "%ENV_RC%"=="0" ( > "%OUT%\wrapper.exit.txt" echo %ENV_RC% & exit /b %ENV_RC% )
set MSBUILDDISABLENODEREUSE=1
cmake --build --preset windows-msvc --config Release --parallel 1 --target hawken-local-udp-tests > "%OUT%\build.stdout.log" 2> "%OUT%\build.stderr.log"
set "BUILD_RC=%ERRORLEVEL%"
> "%OUT%\build.exit.txt" echo %BUILD_RC%
if not "%BUILD_RC%"=="0" ( > "%OUT%\wrapper.exit.txt" echo %BUILD_RC% & exit /b %BUILD_RC% )
"F:\HawkenServer\build\windows-msvc\hawken-local-udp-tests.exe" "--test-case=UDP owning Pawn role*" > "%OUT%\owner.stdout.log" 2> "%OUT%\owner.stderr.log"
set "TEST_RC=%ERRORLEVEL%"
> "%OUT%\owner.exit.txt" echo %TEST_RC%
> "%OUT%\wrapper.exit.txt" echo %TEST_RC%
exit /b %TEST_RC%
