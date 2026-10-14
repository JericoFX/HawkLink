@echo off
setlocal
cd /d F:\HawkenServer
set "OUT=F:\HawkenServer\implementation\evidence\selection-bootstrap-protocol\20261014\gri-hud-validation\gameclass-correction\luna"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 > "%OUT%\devshell.stdout.log" 2> "%OUT%\devshell.stderr.log"
set "VS_RC=%ERRORLEVEL%"
> "%OUT%\devshell.exit.txt" echo %VS_RC%
if not "%VS_RC%"=="0" exit /b %VS_RC%
set MSBUILDDISABLENODEREUSE=1
cmake --build --preset windows-msvc --config Release --parallel 1 --target all > "%OUT%\build.stdout.log" 2> "%OUT%\build.stderr.log"
set "BUILD_RC=%ERRORLEVEL%"
> "%OUT%\build.exit.txt" echo %BUILD_RC%
if not "%BUILD_RC%"=="0" exit /b %BUILD_RC%
"F:\HawkenServer\build\windows-msvc\hawken-server.exe" --help > "%OUT%\help.stdout.log" 2> "%OUT%\help.stderr.log"
set "HELP_RC=%ERRORLEVEL%"
> "%OUT%\help.exit.txt" echo %HELP_RC%
exit /b %HELP_RC%
