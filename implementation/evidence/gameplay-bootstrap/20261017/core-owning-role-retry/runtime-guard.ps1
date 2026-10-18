param([Parameter(Mandatory=$true)][string]$SessionDirectory)
$ErrorActionPreference='Stop'
$trialStart=[DateTime]::UtcNow
$deadline=[DateTimeOffset]::Parse('2026-10-18T00:17:35Z').UtcDateTime
$gamePath='F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe'
$samplePath=Join-Path $SessionDirectory 'resource-samples.jsonl'
$gameIdentity=$null
$reason='unknown'
try {
    while($true) {
        $os=Get-CimInstance Win32_OperatingSystem
        $used=100*(1-$os.FreePhysicalMemory/$os.TotalVisibleMemorySize)
        $row=[ordered]@{utc=[DateTime]::UtcNow.ToString('o');usedPercent=$used;freeKiB=$os.FreePhysicalMemory}
        [IO.File]::AppendAllText($samplePath,(($row|ConvertTo-Json -Compress)+[Environment]::NewLine))
        if($null -eq $gameIdentity) {
            $games=@(Get-CimInstance Win32_Process -Filter "Name='HawkenGame-Win64-Shipping.exe'" | Where-Object {$_.ExecutablePath -ieq $gamePath -and $_.CreationDate.ToUniversalTime() -ge $trialStart})
            if($games.Count -eq 1) {
                $gameIdentity=@{pid=$games[0].ProcessId;creationUtc=$games[0].CreationDate.ToUniversalTime().ToString('o');path=$games[0].ExecutablePath}
                [IO.File]::WriteAllText((Join-Path $SessionDirectory 'guard-game-identity.json'),($gameIdentity|ConvertTo-Json))
            }
        }
        if($used -ge 80){$reason='ram80';break}
        if([DateTime]::UtcNow -ge $deadline){$reason='slot-deadline';break}
        if(Test-Path -LiteralPath (Join-Path $SessionDirectory 'runtime-stop.request')){$reason='parent-request';break}
        Start-Sleep -Seconds 3
    }
} catch {$reason='guard-error'}
finally {
    $closed=$false
    if($null -ne $gameIdentity) {
        $current=Get-CimInstance Win32_Process -Filter "ProcessId=$($gameIdentity.pid)" -ErrorAction SilentlyContinue
        if($current -and $current.ExecutablePath -ieq $gameIdentity.path -and $current.CreationDate.ToUniversalTime().ToString('o') -eq $gameIdentity.creationUtc) {
            $handle=Get-Process -Id $gameIdentity.pid -ErrorAction SilentlyContinue
            if($handle) {try {$null=$handle.CloseMainWindow();if(-not $handle.WaitForExit(5000)){$handle.Kill();$null=$handle.WaitForExit(5000)};$closed=$handle.HasExited}finally{$handle.Dispose()}}
        } else {$closed=$true}
    }
    [IO.File]::WriteAllText((Join-Path $SessionDirectory 'stop.request'),'resource guard closure')
    [IO.File]::WriteAllText((Join-Path $SessionDirectory 'resource-guard-result.json'),(@{utc=[DateTime]::UtcNow.ToString('o');reason=$reason;game=$gameIdentity;gameTerminal=$closed}|ConvertTo-Json -Depth 5))
}
