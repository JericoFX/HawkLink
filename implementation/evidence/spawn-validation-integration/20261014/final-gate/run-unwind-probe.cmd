@echo off
setlocal
set "ROOT=F:\HawkenServer"
set "OUT=%ROOT%\implementation\evidence\spawn-validation-integration\20261014\final-gate\unwind-luna"
set "LLVM=%ROOT%\implementation\tools\llvm-23.1.3\portable\clang+llvm-23.1.3-x86_64-pc-windows-msvc"
set "PROBE=%ROOT%\implementation\evidence\spawn-validation-integration\20261014\final-gate\exception-unwind-probe.cpp"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 > "%OUT%\devshell.stdout.log" 2> "%OUT%\devshell.stderr.log"
set "VS_RC=%ERRORLEVEL%"
> "%OUT%\devshell.exit.txt" echo %VS_RC%
if not "%VS_RC%"=="0" exit /b %VS_RC%
set MSBUILDDISABLENODEREUSE=1
cd /d "%OUT%"
"%LLVM%\bin\clang-cl.exe" /nologo /O2 /Ob1 /EHsc /MT /std:c++20 /W4 /WX /utf-8 /Z7 -fuse-ld=lld "%PROBE%" /Fo"%OUT%\control.obj" /Fe"%OUT%\control.exe" > "%OUT%\control-build.stdout.log" 2> "%OUT%\control-build.stderr.log"
set "CONTROL_BUILD_RC=%ERRORLEVEL%"
> "%OUT%\control-build.exit.txt" echo %CONTROL_BUILD_RC%
if not "%CONTROL_BUILD_RC%"=="0" exit /b %CONTROL_BUILD_RC%
"%LLVM%\bin\clang-cl.exe" /nologo /O2 /Ob1 /EHsc /MT /std:c++20 /W4 /WX /utf-8 /Z7 -fuse-ld=lld /fsanitize=address "%PROBE%" /Fo"%OUT%\asan.obj" /Fe"%OUT%\asan.exe" /link /INCREMENTAL:NO /debug /libpath:"%LLVM%\lib\clang\23\lib\windows" "%LLVM%\lib\clang\23\lib\windows\clang_rt.asan_dynamic-x86_64.lib" /wholearchive:"%LLVM%\lib\clang\23\lib\windows\clang_rt.asan_static_runtime_thunk-x86_64.lib" > "%OUT%\asan-build.stdout.log" 2> "%OUT%\asan-build.stderr.log"
set "ASAN_BUILD_RC=%ERRORLEVEL%"
> "%OUT%\asan-build.exit.txt" echo %ASAN_BUILD_RC%
if not "%ASAN_BUILD_RC%"=="0" exit /b %ASAN_BUILD_RC%
set "PATH=%LLVM%\lib\clang\23\lib\windows;%PATH%"
set "ASAN_OPTIONS=alloc_dealloc_mismatch=1"
set "ASAN_SYMBOLIZER_PATH=%LLVM%\bin\llvm-symbolizer.exe"
"%OUT%\control.exe" > "%OUT%\control-run.stdout.log" 2> "%OUT%\control-run.stderr.log"
set "CONTROL_RUN_RC=%ERRORLEVEL%"
> "%OUT%\control-run.exit.txt" echo %CONTROL_RUN_RC%
"%OUT%\asan.exe" > "%OUT%\asan-run.stdout.log" 2> "%OUT%\asan-run.stderr.log"
set "ASAN_RUN_RC=%ERRORLEVEL%"
> "%OUT%\asan-run.exit.txt" echo %ASAN_RUN_RC%
rem Orchestration only: individual build/run exits decide the diagnostic result.
> "%OUT%\wrapper.exit.txt" echo 0
exit /b 0
