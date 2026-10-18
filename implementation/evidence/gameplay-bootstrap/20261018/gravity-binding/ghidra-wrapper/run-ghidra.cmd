@echo off
setlocal
set "JAVA_HOME=F:\HawkenServer\research\2026-10-10\tools\jdk-21.0.12.1+1"
set "GHIDRA_HEADLESS_MAXMEM=768m"
set "APPDATA=F:\HawkenServer\research\2026-10-10\analysis\ghidra-private-appdata"
set "LOCALAPPDATA=F:\HawkenServer\research\2026-10-10\analysis\ghidra-private-localappdata"
call "C:\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat" "F:\HawkenServer\research\2026-10-10\analysis\ghidra-projects" HawkenMain -process HawkenGame-Win64-Shipping.exe -readOnly -noanalysis -max-cpu 1 -scriptPath "F:\HawkenServer\implementation\evidence\fire-gates-20261017" -preScript EnsureFocusedFunction.java 0x1403fd720 -postScript ExtractNetworkFunctions.java "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\ghidra-wrapper\addresses.txt" "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\ghidra-wrapper\functions.jsonl" -postScript DumpFocusedInstructions.java "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\ghidra-wrapper\instructions.json" 0x1403fd720 -log "F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\ghidra-wrapper\ghidra.log" 1>"F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\ghidra-wrapper\stdout.log" 2>"F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\ghidra-wrapper\stderr.log"
set "rc=%ERRORLEVEL%"
>"F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\ghidra-wrapper\headless.exit" echo %rc%
exit /b %rc%