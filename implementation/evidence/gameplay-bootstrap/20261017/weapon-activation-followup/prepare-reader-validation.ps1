param([Parameter(Mandatory=$true)][ValidateSet('red','green')][string]$Phase)
$ErrorActionPreference='Stop'
$root='F:\HawkenServer'
$base=Join-Path $root 'implementation\evidence\gameplay-bootstrap\20261017\weapon-activation-followup'
$out=Join-Path $base ('reader-'+$Phase)
if(Test-Path -LiteralPath $out){throw 'Evidence directory already exists'}
function Pin([string]$path){[ordered]@{path=$path;sha256=(Get-FileHash -LiteralPath (Join-Path $root $path)).Hash.ToLowerInvariant()}}
$expectedTest='3763eb35eb461bfd0dc91266bcb72dadec417991ec5e54587fe228451c1fdaab'
if((Pin 'tests/live_player_reader_tests.cpp').sha256 -cne $expectedTest){throw 'Frozen test changed'}
$expectedHeader=if($Phase-eq'red'){'7e87547d2c106e7f7a129f9074837049ed041a733514cffcc752e40a0b6344b5'}else{'3f2b70a8fe414852d080744f2953b8cb35114cdb5f5f4a04ef7d4271b7144fa4'}
if((Pin 'tools/live_player_reader.hpp').sha256 -cne $expectedHeader){throw 'Reader phase does not match'}
$old=Get-Content -LiteralPath (Join-Path $root 'implementation\evidence\gameplay-bootstrap\20261017\weapon-rpc-arguments\green\input-freeze.json') -Raw|ConvertFrom-Json
$paths=@($old.sourceInputs.path)+@(Get-ChildItem -LiteralPath (Join-Path $root 'tools') -File|Where-Object{$_.Extension-in'.hpp','.cpp'}|ForEach-Object{'tools/'+$_.Name})
$paths=@($paths|Sort-Object -Unique)
$beforeTouch=@()
if($Phase-eq'green'){
    # Ninja's retained dependency database is incomplete. Force the three
    # transitive consumers to compile; this changes timestamps, never bytes.
    foreach($path in @('tests/live_player_reader_tests.cpp','tests/main_menu_sync_reader_tests.cpp','tools/controller_netfields_probe.cpp')){
        $item=Get-Item -LiteralPath (Join-Path $root $path)
        $beforeTouch+=@{path=$path;utc=$item.LastWriteTimeUtc.ToString('o');sha256=(Pin $path).sha256}
        $item.LastWriteTimeUtc=[DateTime]::UtcNow
    }
}
[void](New-Item -ItemType Directory -Path $out)
$utf8=[Text.UTF8Encoding]::new($false)
if($Phase-eq'red'){
    [void](New-Item -ItemType Directory -Path (Join-Path $out 'accepted-executables'))
    foreach($name in @('hawken-live-player-reader-tests.exe','hawken-main-menu-sync-reader-tests.exe','hawken-controller-netfields-probe.exe')){
        Copy-Item -LiteralPath (Join-Path $root ('build\windows-msvc\'+$name)) -Destination (Join-Path $out ('accepted-executables\'+$name))
    }
}
$tests=@($old.baselineTestExecutables.path)+@('build/windows-msvc/hawken-weapon-fire-rpc-tests.exe')
$freeze=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');phase=$Phase;sourceInputs=@($paths|ForEach-Object{Pin $_});baselineTestExecutables=@($tests|ForEach-Object{Pin $_});protectedCore=(Pin 'build/windows-msvc/hawken-server.exe');protectedMaster=(Pin 'build/windows-msvc/hawken-master.exe');probeBefore=(Pin 'build/windows-msvc/hawken-controller-netfields-probe.exe');forcedConsumerRecompile=$beforeTouch;scope='Read-only observer only; no Core rebuild, client writes or gameplay acceptance'}
[IO.File]::WriteAllText((Join-Path $out 'input-freeze.json'),($freeze|ConvertTo-Json -Depth 12),$utf8)
$targets=if($Phase-eq'red'){'hawken-live-player-reader-tests'}else{'hawken-live-player-reader-tests hawken-main-menu-sync-reader-tests hawken-controller-netfields-probe'}
$lines=@('@echo off','set MSBUILDDISABLENODEREUSE=1',('call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 > "'+$out+'\environment.log" 2>&1'),'if errorlevel 1 exit /b %errorlevel%',('cd /d "'+$root+'"'),('cmake --build build/windows-msvc --config Release --target '+$targets+' --parallel 1 > "'+$out+'\build.log" 2>&1'),'set "buildExit=%errorlevel%"',('echo %buildExit% > "'+$out+'\build.exit"'),'if not "%buildExit%"=="0" exit /b %buildExit%')
if($Phase-eq'red'){
    $lines+=@(('"'+$root+'\build\windows-msvc\hawken-live-player-reader-tests.exe" --no-colors=1 --test-case-exclude="Live inventory script observer*" > "'+$out+'\old-cases.log" 2>&1'),'set "oldExit=%errorlevel%"',('echo %oldExit% > "'+$out+'\old-cases.exit"'),'if not "%oldExit%"=="0" exit /b %oldExit%',('"'+$root+'\build\windows-msvc\hawken-live-player-reader-tests.exe" --no-colors=1 --test-case="Live inventory script observer*" > "'+$out+'\focused.log" 2>&1'),'set "focusedExit=%errorlevel%"',('echo %focusedExit% > "'+$out+'\focused.exit"'),'exit /b %focusedExit%')
}else{
    $lines+=@(('"'+$root+'\build\windows-msvc\hawken-live-player-reader-tests.exe" --no-colors=1 > "'+$out+'\focused.log" 2>&1'),'set "focusedExit=%errorlevel%"',('echo %focusedExit% > "'+$out+'\focused.exit"'),'if not "%focusedExit%"=="0" exit /b %focusedExit%',('ctest --test-dir build/windows-msvc -C Release --output-on-failure --parallel 1 > "'+$out+'\ctest.log" 2>&1'),'set "testExit=%errorlevel%"',('echo %testExit% > "'+$out+'\ctest.exit"'),'exit /b %testExit%')
}
[IO.File]::WriteAllText((Join-Path $out ('run-'+$Phase+'.cmd')),($lines-join "`r`n")+"`r`n",[Text.Encoding]::ASCII)
[ordered]@{directory=$out;freezeSHA256=(Get-FileHash -LiteralPath (Join-Path $out 'input-freeze.json')).Hash;wrapperSHA256=(Get-FileHash -LiteralPath (Join-Path $out ('run-'+$Phase+'.cmd'))).Hash;sourceCount=$paths.Count;baselineTests=$tests.Count;launched=0}|ConvertTo-Json
