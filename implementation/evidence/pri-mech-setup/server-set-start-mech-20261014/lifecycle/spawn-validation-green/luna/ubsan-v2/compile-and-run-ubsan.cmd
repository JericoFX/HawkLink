@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 > "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\devshell.stdout.txt" 2> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\devshell.stderr.txt"
set "rc=%ERRORLEVEL%"
>"F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\devshell.exit.txt" echo %rc%
if not "%rc%"=="0" exit /b %rc%
set MSBUILDDISABLENODEREUSE=1
where cl > "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\where-cl.stdout.txt" 2> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\where-cl.stderr.txt"
set "rc=%ERRORLEVEL%"
>"F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\where-cl.exit.txt" echo %rc%
if not "%rc%"=="0" exit /b %rc%
set "PATH=F:\HawkenServer\implementation\tools\llvm-23.1.3\portable\clang+llvm-23.1.3-x86_64-pc-windows-msvc\bin;%PATH%"
"F:\HawkenServer\implementation\tools\llvm-23.1.3\portable\clang+llvm-23.1.3-x86_64-pc-windows-msvc\bin\clang-cl.exe" /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /Iinclude /external:Ithird_party /external:W0 -fsanitize=undefined /clang:-fno-sanitize-recover=all /Fo:"F:/HawkenServer/implementation/evidence/pri-mech-setup/server-set-start-mech-20261014/lifecycle/spawn-validation-green/luna/ubsan-v2/obj/" /Fe:"F:/HawkenServer/implementation/evidence/pri-mech-setup/server-set-start-mech-20261014/lifecycle/spawn-validation-green/luna/ubsan-v2/spawn_validation_ubsan.exe" /Fd:"F:/HawkenServer/implementation/evidence/pri-mech-setup/server-set-start-mech-20261014/lifecycle/spawn-validation-green/luna/ubsan-v2/spawn_validation_ubsan.pdb" "F:\HawkenServer\tests\test_main.cpp" "F:\HawkenServer\tests\spawn_validation_tests.cpp" "F:\HawkenServer\src\spawn_validation.cpp" > "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\compile.stdout.txt" 2> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\compile.stderr.txt"
set "rc=%ERRORLEVEL%"
>"F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\compile.exit.txt" echo %rc%
if not "%rc%"=="0" exit /b %rc%
set "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
"F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\spawn_validation_ubsan.exe" > "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\run.stdout.txt" 2> "F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\run.stderr.txt"
set "rc=%ERRORLEVEL%"
>"F:\HawkenServer\implementation\evidence\pri-mech-setup\server-set-start-mech-20261014\lifecycle\spawn-validation-green\luna\ubsan-v2\run.exit.txt" echo %rc%
exit /b %rc%
