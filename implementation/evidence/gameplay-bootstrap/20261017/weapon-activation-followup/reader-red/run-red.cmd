@echo off
set MSBUILDDISABLENODEREUSE=1
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-activation-followup\reader-red\environment.log" 2>&1
if errorlevel 1 exit /b %errorlevel%
cd /d "F:\HawkenServer"
cmake --build build/windows-msvc --config Release --target hawken-live-player-reader-tests --parallel 1 > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-activation-followup\reader-red\build.log" 2>&1
set "buildExit=%errorlevel%"
echo %buildExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-activation-followup\reader-red\build.exit"
if not "%buildExit%"=="0" exit /b %buildExit%
"F:\HawkenServer\build\windows-msvc\hawken-live-player-reader-tests.exe" --no-colors=1 --test-case-exclude="Live inventory script observer*" > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-activation-followup\reader-red\old-cases.log" 2>&1
set "oldExit=%errorlevel%"
echo %oldExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-activation-followup\reader-red\old-cases.exit"
if not "%oldExit%"=="0" exit /b %oldExit%
"F:\HawkenServer\build\windows-msvc\hawken-live-player-reader-tests.exe" --no-colors=1 --test-case="Live inventory script observer*" > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-activation-followup\reader-red\focused.log" 2>&1
set "focusedExit=%errorlevel%"
echo %focusedExit% > "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-activation-followup\reader-red\focused.exit"
exit /b %focusedExit%
