param([string]$ExpectedFreezeSha256,[int]$DeadlineSeconds=90,[long]$MaxOwnedWorkingSet=2147483648)
$ErrorActionPreference='Stop'
$root='F:\HawkenServer'
$outDir=Join-Path $root 'implementation\evidence\gameplay-bootstrap\20261018\gravity-binding\ghidra-wrapper'
$freezePath=Join-Path $outDir 'input-freeze.json'
foreach($artifact in @('functions.jsonl','instructions.json','stdout.log','stderr.log','headless.exit','guard-result.json','ghidra.log')){if(Test-Path -LiteralPath (Join-Path $outDir $artifact)){throw ('Retained output exists; inspect prior job before launch: '+$artifact)}}
function Get-RamSample {
    $os=Get-CimInstance Win32_OperatingSystem
    [ordered]@{utc=[DateTime]::UtcNow.ToString('o');usedPercent=100.0*(1-$os.FreePhysicalMemory/$os.TotalVisibleMemorySize);freeBytes=[int64]$os.FreePhysicalMemory*1KB}
}
function Get-ProcessPath($Handle) { try { $Handle.Refresh(); return $Handle.MainModule.FileName } catch { return $null } }
$freezeHash=(Get-FileHash -LiteralPath $freezePath -Algorithm SHA256).Hash.ToLowerInvariant()
if(-not $ExpectedFreezeSha256 -or $freezeHash -ne $ExpectedFreezeSha256.ToLowerInvariant()){throw 'Freeze hash mismatch; no process launched'}
$freeze=Get-Content -LiteralPath $freezePath -Raw | ConvertFrom-Json
foreach($pin in $freeze.inputs){$pinPath=[string]$pin.path;if(-not [System.IO.Path]::IsPathRooted($pinPath)){$pinPath=Join-Path $root $pinPath};$actual=(Get-FileHash -LiteralPath $pinPath -Algorithm SHA256).Hash.ToLowerInvariant();if($actual -ne $pin.sha256.ToLowerInvariant()){throw ('Input pin mismatch: '+$pin.path)}}
$admission=Get-RamSample
$admission | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outDir 'admission.json') -Encoding utf8
if($admission.usedPercent -ge 70 -or $admission.freeBytes -lt 10GB){throw 'RAM admission failed; no process launched'}
$busy=@(Get-CimInstance Win32_Process | Where-Object { $_.Name -in @('cmake.exe','ninja.exe','cl.exe','link.exe','ctest.exe','MSBuild.exe') -or (($_.Name -in @('java.exe','javaw.exe')) -and $_.CommandLine -match '(?i)(ghidra|analyzeHeadless)') })
if($busy.Count){throw 'Incompatible compiler/Ghidra job detected; no process launched'}
$handles=@{}
$guardTriggered=$false
$deadlineTriggered=$false
$launcher=$null
$startedUtc=$null
$samples=[Collections.Generic.List[object]]::new()
$failure=$null
$closed=@()
try {
    $cmd=Join-Path $outDir 'run-ghidra.cmd'
    $launcher=Start-Process -FilePath $env:ComSpec -ArgumentList ('/d /c "'+$cmd+'"') -WorkingDirectory $root -WindowStyle Hidden -PassThru
    $startedUtc=$launcher.StartTime.ToUniversalTime()
    $launcherPath=Get-ProcessPath $launcher
    if(-not $launcherPath -or $launcherPath -ine $env:ComSpec){throw 'Launcher identity mismatch after start'}
    $handles[[int]$launcher.Id]=[pscustomobject]@{handle=$launcher;creation=$startedUtc;path=$launcherPath;depth=0}
    $deadline=[DateTime]::UtcNow.AddSeconds($DeadlineSeconds)
    while($true){
        $all=@(Get-CimInstance Win32_Process)
        for($level=0;$level -lt 8;$level++){
            foreach($row in $all){
                $processId=[int]$row.ProcessId
                if($handles.ContainsKey($processId)){continue}
                $parentPid=[int]$row.ParentProcessId
                if(-not $handles.ContainsKey($parentPid)){continue}
                $parent=$handles[$parentPid]
                if($parent.handle.HasExited){continue}
                if(-not $row.CreationDate -or $row.CreationDate.ToUniversalTime() -lt $parent.creation){continue}
                $handle=Get-Process -Id $processId -ErrorAction SilentlyContinue
                if(-not $handle){continue}
                $path=Get-ProcessPath $handle
                if($handle.HasExited -or -not $path -or $path -ine $row.ExecutablePath -or [math]::Abs(($handle.StartTime.ToUniversalTime()-$row.CreationDate.ToUniversalTime()).TotalSeconds) -gt 1){$handle.Dispose();continue}
                try{$handle.PriorityClass='BelowNormal'}catch{}
                $handles[$processId]=[pscustomobject]@{handle=$handle;creation=$handle.StartTime.ToUniversalTime();path=$path;depth=$parent.depth+1}
            }
        }
        $active=@($handles.Values | Where-Object {-not $_.handle.HasExited})
        $ram=Get-RamSample
        $ownedBytes=0L
        foreach($node in $active){try{$node.handle.Refresh();$ownedBytes += [int64]$node.handle.WorkingSet64}catch{}}
        $ram.ownedWorkingSetBytes=$ownedBytes
        $samples.Add($ram)
        $guardTriggered=$ram.usedPercent -ge 80 -or $ownedBytes -gt $MaxOwnedWorkingSet
        $deadlineTriggered=[DateTime]::UtcNow -ge $deadline
        if($guardTriggered -or $deadlineTriggered -or $active.Count -eq 0){break}
        Start-Sleep -Milliseconds 2000
    }
    if($launcher -and $launcher.HasExited){$launcher.WaitForExit()}
} catch { $failure=$_.Exception.Message }
finally {
    foreach($node in @($handles.Values | Sort-Object depth -Descending)){
        $h=$node.handle
        try {
            if(-not $h.HasExited){
                $current=Get-CimInstance Win32_Process -Filter "ProcessId = $($h.Id)" -ErrorAction SilentlyContinue
                if($current -and $current.CreationDate -and [math]::Abs(($current.CreationDate.ToUniversalTime()-$node.creation).TotalSeconds) -lt 1 -and $current.ExecutablePath -ieq $node.path){$h.Kill();[void]$h.WaitForExit(5000)}
            }
            $closed+=@{pid=$h.Id;creationUtc=$node.creation.ToString('o');path=$node.path;terminal=$h.HasExited;exitCode=if($h.HasExited){$h.ExitCode}else{$null}}
        } catch { $closed+=@{pid=$h.Id;creationUtc=$node.creation.ToString('o');path=$node.path;terminal=$false;cleanupError=$_.Exception.Message} }
        finally {if($h -ne $launcher){$h.Dispose()}}
    }
    $record=[ordered]@{phase='gravity-binding-single-function';utc=[DateTime]::UtcNow.ToString('o');freezeSha256=$freezeHash;admission=$admission;guardTriggered=$guardTriggered;deadlineTriggered=$deadlineTriggered;startedUtc=if($startedUtc){$startedUtc.ToString('o')}else{$null};processes=$closed;samples=$samples.ToArray();launcherExit=if($launcher -and $launcher.HasExited){$launcher.ExitCode}else{$null};finalRam=Get-RamSample;failure=$failure}
    $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outDir 'guard-result.json') -Encoding utf8
    if($launcher){$launcher.Dispose()}
}
if($guardTriggered -or $deadlineTriggered -or $failure){exit 4}
if(-not $record.startedUtc){exit 3}
if($record.launcherExit -ne 0){exit 1}
exit 0