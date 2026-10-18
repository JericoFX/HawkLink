param(
    [Parameter(Mandatory=$true)][ValidateSet('red','green')][string]$Phase,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [Parameter(Mandatory=$true)][string]$InputFreezePath,
    [Parameter(Mandatory=$true)][string]$ExpectedInputFreezeSha256,
    [Parameter(Mandatory=$true)][string]$WrapperPath,
    [Parameter(Mandatory=$true)][string]$ExpectedWrapperSha256,
    [ValidateRange(30,3600)][int]$DeadlineSeconds = 900
)
$ErrorActionPreference='Stop'
$root='F:\HawkenServer'
$evidenceRoot=[IO.Path]::GetFullPath((Join-Path $root 'implementation\evidence'))
function Assert-UnderRoot([string]$path,[string]$allowedRoot) {
    $full=[IO.Path]::GetFullPath($path)
    $prefix=$allowedRoot.TrimEnd('\')+'\'
    if(-not $full.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw "Path outside allowed evidence root: $full"}
    return $full
}
function Get-FileSha([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-RamSample {
    $os=Get-CimInstance -ClassName Win32_OperatingSystem
    $total=[int64]$os.TotalVisibleMemorySize*1KB
    $free=[int64]$os.FreePhysicalMemory*1KB
    [ordered]@{utc=[DateTime]::UtcNow.ToString('o');totalBytes=$total;freeBytes=$free;usedPercent=[Math]::Round((100.0*($total-$free)/$total),2)}
}
function Get-HeavyProcesses {
    $names=@('cl.exe','cmake.exe','ninja.exe','ctest.exe','msbuild.exe','link.exe')
    @(Get-CimInstance -ClassName Win32_Process | Where-Object { $names -contains $_.Name.ToLowerInvariant() } | ForEach-Object {
        [ordered]@{pid=[int]$_.ProcessId;parentPid=[int]$_.ParentProcessId;name=$_.Name;path=$_.ExecutablePath;creationUtc=if($_.CreationDate){$_.CreationDate.ToUniversalTime().ToString('o')}else{$null};workingSetBytes=[int64]$_.WorkingSetSize}
    })
}
function Get-InputPins($freeze) {
    $rows=@()
    foreach($entry in @($freeze.sourceInputs)) {
        $full=Join-Path $root ([string]$entry.path)
        if(-not (Test-Path -LiteralPath $full -PathType Leaf)){$actual=$null}else{$actual=Get-FileSha $full}
        $rows+=,[ordered]@{path=[string]$entry.path;expected=[string]$entry.sha256;actual=$actual;matches=($actual -ceq [string]$entry.sha256)}
    }
    $rows
}
function Get-TestExePins($freeze) {
    $rows=@()
    foreach($entry in @($freeze.baselineTestExecutables)) {
        $full=Join-Path $root ([string]$entry.path)
        if(-not (Test-Path -LiteralPath $full -PathType Leaf)){$actual=$null}else{$actual=Get-FileSha $full}
        $rows+=,[ordered]@{path=[string]$entry.path;expected=[string]$entry.sha256;actual=$actual;matches=($actual -ceq [string]$entry.sha256)}
    }
    $rows
}
function Get-ProcessTree([int]$RootPid) {
    $all=@(Get-CimInstance -ClassName Win32_Process)
    $byPid=@{}
    foreach($row in $all){$byPid[[int]$row.ProcessId]=$row}
    $found=@{}
    $depthByPid=@{$RootPid=0}
    $queue=New-Object 'System.Collections.Generic.Queue[int]'
    $queue.Enqueue($RootPid)
    while($queue.Count -gt 0){
        $parent=$queue.Dequeue()
        $depth=$depthByPid[$parent]
        foreach($row in $all){
            $child=[int]$row.ProcessId
            if([int]$row.ParentProcessId -eq $parent -and -not $depthByPid.ContainsKey($child)){
                $depthByPid[$child]=$depth+1
                $queue.Enqueue($child)
            }
        }
    }
    foreach($row in $all){
        $pidValue=[int]$row.ProcessId
        if($depthByPid.ContainsKey($pidValue)){
            $parentRow=$byPid[[int]$row.ParentProcessId]
            $found[$pidValue]=[ordered]@{pid=$pidValue;parentPid=[int]$row.ParentProcessId;depth=[int]$depthByPid[$pidValue];name=$row.Name;path=$row.ExecutablePath;creationUtc=if($row.CreationDate){$row.CreationDate.ToUniversalTime().ToString('o')}else{$null};parentCreationUtc=if($parentRow -and $parentRow.CreationDate){$parentRow.CreationDate.ToUniversalTime().ToString('o')}else{$null};parentPath=if($parentRow){$parentRow.ExecutablePath}else{$null};workingSetBytes=[int64]$row.WorkingSetSize}
        }
    }
    @($found.Values | Sort-Object depth -Descending)
}
function Stop-OwnedTree([int]$RootPid,[string]$RootCreationUtc,[string]$RootPath,[System.Diagnostics.Process]$OriginalProcess) {
    $stopped=@();$skipped=@()
    if(-not $OriginalProcess -or $OriginalProcess.Id -ne $RootPid -or $OriginalProcess.HasExited){return [ordered]@{rootVerified=$false;rootReason='Original launcher process handle is absent or exited';stopped=$stopped;skipped=$skipped}}
    $handleCreation=$OriginalProcess.StartTime.ToUniversalTime()
    $handlePath=[IO.Path]::GetFullPath($OriginalProcess.MainModule.FileName)
    if($handleCreation.ToString('o') -cne $RootCreationUtc -or $handlePath -ine [IO.Path]::GetFullPath($RootPath)){return [ordered]@{rootVerified=$false;rootReason='Original process handle creation time or executable path did not match';stopped=$stopped;skipped=$skipped}}
    $rootCheck=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $RootPid" -ErrorAction SilentlyContinue
    if(-not $rootCheck){return [ordered]@{rootVerified=$false;rootReason='Launcher PID is absent';stopped=$stopped;skipped=$skipped}}
    $rootCheckCreation=if($rootCheck.CreationDate){$rootCheck.CreationDate.ToUniversalTime()}else{$null}
    if($null -eq $rootCheckCreation -or [Math]::Abs(($rootCheckCreation-$handleCreation).TotalSeconds) -gt 1.0 -or $rootCheck.ExecutablePath -ine $RootPath){return [ordered]@{rootVerified=$false;rootReason='Launcher handle did not match CIM creation time or executable path';stopped=$stopped;skipped=$skipped}}
    $tree=Get-ProcessTree $RootPid
    $byPid=@{};foreach($node in $tree){$byPid[[int]$node.pid]=$node}
    foreach($item in $tree){
        $current=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($item.pid)" -ErrorAction SilentlyContinue
        if(-not $current){continue}
        $creation=if($current.CreationDate){$current.CreationDate.ToUniversalTime().ToString('o')}else{$null}
        $expectedPath=if($item.pid -eq $RootPid){$RootPath}else{$item.path}
        $stillOwned=-not [string]::IsNullOrWhiteSpace($item.creationUtc) -and -not [string]::IsNullOrWhiteSpace($expectedPath) -and -not [string]::IsNullOrWhiteSpace($current.ExecutablePath) -and $creation -ceq $item.creationUtc -and $current.ExecutablePath -ieq $expectedPath
        if($item.pid -eq $RootPid){$stillOwned=$stillOwned -and -not $OriginalProcess.HasExited -and $OriginalProcess.Id -eq $RootPid}
        else {
            $parentNow=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($item.parentPid)" -ErrorAction SilentlyContinue
            $parentCreation=if($parentNow -and $parentNow.CreationDate){$parentNow.CreationDate.ToUniversalTime().ToString('o')}else{$null}
            $stillOwned=$stillOwned -and $null -ne $byPid[[int]$item.parentPid] -and -not [string]::IsNullOrWhiteSpace($item.parentCreationUtc) -and -not [string]::IsNullOrWhiteSpace($item.parentPath) -and -not [string]::IsNullOrWhiteSpace($parentNow.ExecutablePath) -and $parentCreation -ceq $item.parentCreationUtc -and $parentNow.ExecutablePath -ieq $item.parentPath
        }
        if(-not $stillOwned){$skipped+=@{pid=$item.pid;reason='PID creation time/path or verified parent lineage changed'};continue}
        $handle=$null
        try {
            $handle=if($item.pid -eq $RootPid){$OriginalProcess}else{Get-Process -Id $item.pid -ErrorAction Stop}
            $confirm=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($item.pid)" -ErrorAction SilentlyContinue
            $confirmCreation=if($confirm -and $confirm.CreationDate){$confirm.CreationDate.ToUniversalTime().ToString('o')}else{$null}
            if(-not $confirm -or $confirmCreation -cne $creation -or $confirm.ExecutablePath -ine $current.ExecutablePath -or $handle.HasExited){
                $skipped+=@{pid=$item.pid;reason='Identity changed after process handle was opened'}
                continue
            }
            Stop-Process -InputObject $handle -Force -ErrorAction Stop
            $terminal=$handle.WaitForExit(5000)
            $stopped+=@{pid=$item.pid;creationUtc=$creation;path=$current.ExecutablePath;terminal=$terminal}
        }
        catch { $skipped+=@{pid=$item.pid;reason=$_.Exception.Message} }
        finally {if($handle -and $item.pid -ne $RootPid){$handle.Dispose()}}
    }
    [ordered]@{rootVerified=$true;rootReason=$null;stopped=$stopped;skipped=$skipped}
}
function Set-OwnedTreePriority([int]$RootPid,[string]$RootCreationUtc,[string]$RootPath,[System.Diagnostics.Process]$OriginalProcess) {
    if(-not $OriginalProcess -or $OriginalProcess.Id -ne $RootPid -or $OriginalProcess.HasExited){return @([ordered]@{pid=$RootPid;applied=$false;reason='Original launcher process handle is absent or exited'})}
    $handleCreation=$OriginalProcess.StartTime.ToUniversalTime()
    $handlePath=[IO.Path]::GetFullPath($OriginalProcess.MainModule.FileName)
    if($handleCreation.ToString('o') -cne $RootCreationUtc -or $handlePath -ine [IO.Path]::GetFullPath($RootPath)){return @([ordered]@{pid=$RootPid;applied=$false;reason='Original process handle creation time or path changed'})}
    $rootNow=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $RootPid" -ErrorAction SilentlyContinue
    $rootNowCreation=if($rootNow -and $rootNow.CreationDate){$rootNow.CreationDate.ToUniversalTime()}else{$null}
    if(-not $rootNow -or $null -eq $rootNowCreation -or [Math]::Abs(($rootNowCreation-$handleCreation).TotalSeconds) -gt 1.0 -or $rootNow.ExecutablePath -ine $RootPath){return @([ordered]@{pid=$RootPid;applied=$false;reason='Launcher identity not verified'})}
    $rows=@()
    foreach($item in (Get-ProcessTree $RootPid)){
        $current=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($item.pid)" -ErrorAction SilentlyContinue
        if(-not $current){$rows+=,[ordered]@{pid=$item.pid;applied=$false;reason='Process absent'};continue}
        $creation=if($current.CreationDate){$current.CreationDate.ToUniversalTime().ToString('o')}else{$null}
        $expectedPath=if($item.pid -eq $RootPid){$RootPath}else{$item.path}
        $valid=-not [string]::IsNullOrWhiteSpace($item.creationUtc) -and -not [string]::IsNullOrWhiteSpace($expectedPath) -and -not [string]::IsNullOrWhiteSpace($current.ExecutablePath) -and $creation -ceq $item.creationUtc -and $current.ExecutablePath -ieq $expectedPath
        if($item.pid -ne $RootPid){
            $parentNow=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($item.parentPid)" -ErrorAction SilentlyContinue
            $parentCreation=if($parentNow -and $parentNow.CreationDate){$parentNow.CreationDate.ToUniversalTime().ToString('o')}else{$null}
            $valid=$valid -and $null -ne $parentNow -and -not [string]::IsNullOrWhiteSpace($item.parentCreationUtc) -and -not [string]::IsNullOrWhiteSpace($item.parentPath) -and -not [string]::IsNullOrWhiteSpace($parentNow.ExecutablePath) -and $parentCreation -ceq $item.parentCreationUtc -and $parentNow.ExecutablePath -ieq $item.parentPath
        }
        if(-not $valid){$rows+=,[ordered]@{pid=$item.pid;applied=$false;reason='PID/path/lineage changed'};continue}
        $p=$null
        try {$p=Get-Process -Id $item.pid -ErrorAction Stop;$p.PriorityClass='BelowNormal';$rows+=,[ordered]@{pid=$item.pid;applied=$true;priorityClass=$p.PriorityClass.ToString()}}
        catch {$rows+=,[ordered]@{pid=$item.pid;applied=$false;reason=$_.Exception.Message}}
        finally {if($p){$p.Dispose()}}
    }
    @($rows)
}
function Write-Json([string]$path,$value) {
    $json=ConvertTo-Json -InputObject $value -Depth 16
    [IO.File]::WriteAllText($path,$json+"`n",[Text.UTF8Encoding]::new($false))
}
$freezeFull=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($InputFreezePath)){$InputFreezePath}else{Join-Path $root $InputFreezePath}))
$wrapperFull=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($WrapperPath)){$WrapperPath}else{Join-Path $root $WrapperPath}))
$outFull=Assert-UnderRoot (Join-Path $root $OutputDirectory) $evidenceRoot
if(Test-Path -LiteralPath $outFull){throw "Output directory already exists: $outFull"}
if(-not (Test-Path -LiteralPath $freezeFull -PathType Leaf) -or -not (Test-Path -LiteralPath $wrapperFull -PathType Leaf)){throw 'Freeze or wrapper is missing'}
$freezeSha=Get-FileSha $freezeFull
$wrapperSha=Get-FileSha $wrapperFull
if($freezeSha -cne $ExpectedInputFreezeSha256.ToLowerInvariant()){throw 'Input freeze SHA mismatch'}
if($wrapperSha -cne $ExpectedWrapperSha256.ToLowerInvariant()){throw 'Wrapper SHA mismatch'}
$freeze=Get-Content -LiteralPath $freezeFull -Raw|ConvertFrom-Json
if(@($freeze.sourceInputs).Count -ne 225 -or @($freeze.baselineTestExecutables).Count -ne 60){throw 'Expected 225 source pins and 60 baseline test executables'}
$preInputs=Get-InputPins $freeze
$preExes=Get-TestExePins $freeze
$preRam=Get-RamSample
$heavy=Get-HeavyProcesses
$corePre=Get-FileSha (Join-Path $root ([string]$freeze.protectedCore.path))
$masterPre=Get-FileSha (Join-Path $root ([string]$freeze.protectedMaster.path))
$probePre=Get-FileSha (Join-Path $root ([string]$freeze.probeBefore.path))
$coreStablePre=($corePre -ceq [string]$freeze.protectedCore.sha256)
$masterStablePre=($masterPre -ceq [string]$freeze.protectedMaster.sha256)
$probeStablePre=($Phase -ne 'red' -or $probePre -ceq [string]$freeze.probeBefore.sha256)
$admitted=($preRam.usedPercent -lt 75 -and $preRam.freeBytes -ge 8GB -and $heavy.Count -eq 0 -and (@($preInputs|Where-Object{-not $_.matches}).Count -eq 0) -and (@($preExes|Where-Object{-not $_.matches}).Count -eq 0) -and $coreStablePre -and $masterStablePre -and $probeStablePre)
[void](New-Item -ItemType Directory -Path $outFull)
$preflight=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');phase=$Phase;freezePath=$freezeFull;freezeSha256=$freezeSha;wrapperPath=$wrapperFull;wrapperSha256=$wrapperSha;admission=$admitted;ram=$preRam;incompatibleBuildProcesses=$heavy;sourcePinCount=$preInputs.Count;sourcePinDrift=@($preInputs|Where-Object{-not $_.matches});testExecutableCount=$preExes.Count;testExecutableDrift=@($preExes|Where-Object{-not $_.matches});protectedCore=[ordered]@{path=$freeze.protectedCore.path;expected=$freeze.protectedCore.sha256;actual=$corePre;matches=$coreStablePre};protectedMaster=[ordered]@{path=$freeze.protectedMaster.path;expected=$freeze.protectedMaster.sha256;actual=$masterPre;matches=$masterStablePre};probeBefore=[ordered]@{path=$freeze.probeBefore.path;expected=$freeze.probeBefore.sha256;actual=$probePre;matches=($probePre -ceq [string]$freeze.probeBefore.sha256);requiredStable=($Phase -eq 'red')}}
Write-Json (Join-Path $outFull 'preflight.json') $preflight
Write-Json (Join-Path $outFull 'source-pins-before.json') $preInputs
Write-Json (Join-Path $outFull 'test-exe-pins-before.json') $preExes
if(-not $admitted){Write-Json (Join-Path $outFull 'run-summary.json') ([ordered]@{phase=$Phase;status='not-launched';reason='Admission gate failed';preflight='preflight.json'});exit 3}
# Parent-authorized wrapper and task-local no-node-reuse setting are preserved; never edit validation inputs here.
$oldNoReuse=$env:MSBUILDDISABLENODEREUSE
$oldPriority=(Get-Process -Id $PID).PriorityClass.ToString()
try {
    (Get-Process -Id $PID).PriorityClass='BelowNormal'
    $env:MSBUILDDISABLENODEREUSE='1'
    $cmdArgs='/d /c "'+$wrapperFull+'"'
    $rootPath=[IO.Path]::GetFullPath($env:ComSpec)
    $proc=Start-Process -FilePath $env:ComSpec -ArgumentList $cmdArgs -WorkingDirectory $root -PassThru -WindowStyle Hidden
    $rootCreation=$proc.StartTime.ToUniversalTime().ToString('o')
    $procInfo=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($proc.Id)"
    $cimCreation=if($procInfo.CreationDate){$procInfo.CreationDate.ToUniversalTime()}else{$null}
    if($null -eq $cimCreation -or [Math]::Abs(($cimCreation-$proc.StartTime.ToUniversalTime()).TotalSeconds) -gt 1.0 -or $procInfo.ExecutablePath -ine $rootPath){throw 'Launcher identity did not match its original process handle'}
    $child=(Get-Process -Id $proc.Id -ErrorAction Stop)
    $child.PriorityClass='BelowNormal'
    $startedUtc=[DateTime]::UtcNow.ToString('o')
    $launch=[ordered]@{pid=$proc.Id;creationUtc=$rootCreation;path=$procInfo.ExecutablePath;priorityClass=$child.PriorityClass.ToString();wrapper=$wrapperFull;startedUtc=$startedUtc}
    Write-Json (Join-Path $outFull 'launch.json') $launch
    $priorityObservations=Set-OwnedTreePriority $proc.Id $rootCreation $rootPath $proc
    Write-Json (Join-Path $outFull 'priority-observations.json') $priorityObservations
    $samples=New-Object 'System.Collections.Generic.List[object]'
    $deadline=[DateTime]::UtcNow.AddSeconds($DeadlineSeconds)
    $guardTriggered=$false;$deadlineTriggered=$false;$termination=[ordered]@{stopped=@();skipped=@()}
    while(-not $proc.WaitForExit(2000)){
        $sample=Get-RamSample
        $tree=Get-ProcessTree $proc.Id
        $treeBytes=0L;foreach($node in $tree){$treeBytes+=[int64]$node.workingSetBytes}
        $sample['ownedTreeWorkingSetBytes']=$treeBytes
        $sample['priorityObservations']=Set-OwnedTreePriority $proc.Id $rootCreation $rootPath $proc
        $samples.Add($sample)
        if($sample.usedPercent -ge 80){$guardTriggered=$true;$termination=Stop-OwnedTree $proc.Id $rootCreation $rootPath $proc;break}
        if([DateTime]::UtcNow -ge $deadline){$deadlineTriggered=$true;$termination=Stop-OwnedTree $proc.Id $rootCreation $rootPath $proc;break}
    }
    $launcherTerminal=$proc.WaitForExit(5000)
    if($launcherTerminal){$proc.Refresh();$launcherExit=$proc.ExitCode}else{$launcherExit=$null}
    Write-Json (Join-Path $outFull 'resource-samples.json') $samples.ToArray()
    $wrapperDir=Split-Path -Parent $wrapperFull
    $exitFiles=@()
    foreach($file in @(Get-ChildItem -LiteralPath $wrapperDir -File -Filter '*.exit')){
        $text=(Get-Content -LiteralPath $file.FullName -Raw).Trim()
        [int]$number=0;$parsed=[int]::TryParse($text,[ref]$number)
        $writtenUtc=$file.LastWriteTimeUtc.ToString('o')
        $exitFiles+=,[ordered]@{path=$file.FullName;value=if($parsed){$number}else{$null};raw=$text;lastWriteUtc=$writtenUtc;writtenAfterLaunch=($file.LastWriteTimeUtc -ge [DateTime]::Parse($startedUtc).ToUniversalTime())}
    }
    $logFiles=@(Get-ChildItem -LiteralPath $wrapperDir -File -Filter '*.log' | ForEach-Object {[ordered]@{path=$_.FullName;sha256=(Get-FileSha $_.FullName);length=$_.Length}})
    $doctest=@()
    foreach($log in @(Get-ChildItem -LiteralPath $wrapperDir -File -Filter '*.log')){
        $text=Get-Content -LiteralPath $log.FullName -Raw
        $caseMatches=[regex]::Matches($text,'(?m)test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed\s*\|\s*(\d+) skipped')
        $assertMatches=[regex]::Matches($text,'(?m)assertions:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed')
        if($caseMatches.Count -or $assertMatches.Count){$doctest+=,[ordered]@{file=$log.Name;caseSummaries=@($caseMatches|ForEach-Object{@{total=[int]$_.Groups[1].Value;passed=[int]$_.Groups[2].Value;failed=[int]$_.Groups[3].Value;skipped=[int]$_.Groups[4].Value}});assertionSummaries=@($assertMatches|ForEach-Object{@{total=[int]$_.Groups[1].Value;passed=[int]$_.Groups[2].Value;failed=[int]$_.Groups[3].Value}})}}
    }
    $lastTestPath=Join-Path $root 'build/windows-msvc/Testing/Temporary/LastTest.log'
    $lastTestCopy=$null;$lastTestSha=$null
    $lastTestCurrent=$false
    if($Phase -eq 'green' -and (Test-Path -LiteralPath $lastTestPath -PathType Leaf)){$lastItem=Get-Item -LiteralPath $lastTestPath;$lastTestCurrent=($lastItem.LastWriteTimeUtc -ge [DateTime]::Parse($startedUtc).ToUniversalTime());$lastTestCopy=Join-Path $outFull 'LastTest.log.raw';Copy-Item -LiteralPath $lastTestPath -Destination $lastTestCopy;$lastTestSha=Get-FileSha $lastTestCopy}
    $postInputs=Get-InputPins $freeze
    $postExes=Get-TestExePins $freeze
    Write-Json (Join-Path $outFull 'source-pins-after.json') $postInputs
    Write-Json (Join-Path $outFull 'test-exe-pins-after.json') $postExes
    $expectedChanged=if($Phase -eq 'red'){@('hawken-live-player-reader-tests.exe')}else{@('hawken-live-player-reader-tests.exe','hawken-main-menu-sync-reader-tests.exe')}
    $unexpectedExeChanges=@($postExes|Where-Object{$_.actual -cne $_.expected -and $expectedChanged -notcontains [IO.Path]::GetFileName($_.path)})
    $ctestLog=Join-Path $wrapperDir 'ctest.log'
    $ctestSummary=$null
    if(Test-Path -LiteralPath $ctestLog){$ctext=Get-Content -LiteralPath $ctestLog -Raw;$ctestSummary=[ordered]@{passedTestLines=[regex]::Matches($ctext,'(?m)^\s*\d+/\d+ Test #\d+: .*?\.\.\. Passed').Count;failedTestLines=[regex]::Matches($ctext,'(?m)^\s*\d+/\d+ Test #\d+: .*?\.\.\.\s*\*\*\*Failed').Count;summaryLine=([regex]::Matches($ctext,'(?m)^\s*\d+% tests passed,.*$')|ForEach-Object Value|Select-Object -Last 1)}}
    $coreNow=Get-FileSha (Join-Path $root ([string]$freeze.protectedCore.path))
    $masterNow=Get-FileSha (Join-Path $root ([string]$freeze.protectedMaster.path))
    $probeNow=Get-FileSha (Join-Path $root ([string]$freeze.probeBefore.path))
    $summary=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');phase=$Phase;status=if($guardTriggered){'stopped-memory-guard'}elseif($deadlineTriggered){'stopped-deadline'}else{'wrapper-terminal'};launcherTerminal=$launcherTerminal;launcherExit=$launcherExit;guardTriggered=$guardTriggered;deadlineTriggered=$deadlineTriggered;deadlineSeconds=$DeadlineSeconds;launch=$launch;wrapperExitFiles=$exitFiles;doctestSummaries=$doctest;ctestSummary=$ctestSummary;lastTestLogCopy=$lastTestCopy;lastTestLogSha256=$lastTestSha;lastTestLogWrittenDuringRun=$lastTestCurrent;logFiles=$logFiles;sourceDrift=@($postInputs|Where-Object{-not $_.matches});unexpectedTestExeChanges=$unexpectedExeChanges;protectedCore=[ordered]@{path=$freeze.protectedCore.path;expected=$freeze.protectedCore.sha256;actual=$coreNow;matches=($coreNow -ceq $freeze.protectedCore.sha256)};protectedMaster=[ordered]@{path=$freeze.protectedMaster.path;expected=$freeze.protectedMaster.sha256;actual=$masterNow;matches=($masterNow -ceq $freeze.protectedMaster.sha256)};probeBefore=[ordered]@{path=$freeze.probeBefore.path;expected=$freeze.probeBefore.sha256;actual=$probeNow;matches=($probeNow -ceq [string]$freeze.probeBefore.sha256);expectedToRemainStable=($Phase -eq 'red')};termination=$termination;finalRam=Get-RamSample;taskOwnedProcessesRemaining=(Get-ProcessTree $proc.Id)}
    Write-Json (Join-Path $outFull 'run-summary.json') $summary
    if($oldNoReuse -eq $null){Remove-Item Env:MSBUILDDISABLENODEREUSE -ErrorAction SilentlyContinue}else{$env:MSBUILDDISABLENODEREUSE=$oldNoReuse}
    (Get-Process -Id $PID -ErrorAction SilentlyContinue).PriorityClass=$oldPriority
    if($guardTriggered -or $deadlineTriggered){exit 4}
    exit $launcherExit
}
catch {
    $caught=$_.Exception.Message
    $catchTermination=[ordered]@{attempted=$false;rootVerified=$false;stopped=@();skipped=@()}
    $catchWait=$null;$catchExit=$null;$catchFinalRam=$null;$catchTree=@()
    if($proc){
        try {
            if($rootCreation -and $rootPath){$catchTermination=Stop-OwnedTree $proc.Id $rootCreation $rootPath $proc;$catchTermination['attempted']=$true}
            if(-not $proc.HasExited){$catchWait=$proc.WaitForExit(5000)}else{$catchWait=$true}
            if($catchWait){$proc.Refresh();$catchExit=$proc.ExitCode}
            $catchTree=Get-ProcessTree $proc.Id
        } catch {$catchTermination['cleanupError']=$_.Exception.Message}
        finally {$catchFinalRam=Get-RamSample}
    }
    $failure=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');phase=$Phase;error=$caught;ramAtError=Get-RamSample;rootPid=if($proc){$proc.Id}else{$null};rootCreationUtc=$rootCreation;rootExecutablePath=$rootPath;cleanup=$catchTermination;launcherExited=$catchWait;launcherExit=$catchExit;finalRam=$catchFinalRam;remainingTree=$catchTree}
    Write-Json (Join-Path $outFull 'runner-error.json') $failure
    if($oldNoReuse -eq $null){Remove-Item Env:MSBUILDDISABLENODEREUSE -ErrorAction SilentlyContinue}else{$env:MSBUILDDISABLENODEREUSE=$oldNoReuse}
    if($oldPriority){try{(Get-Process -Id $PID -ErrorAction SilentlyContinue).PriorityClass=$oldPriority}catch{}}
    throw
}
finally {
    if($child -and $child -is [IDisposable]){$child.Dispose()}
    if($proc -and $proc -is [IDisposable]){$proc.Dispose()}
}
