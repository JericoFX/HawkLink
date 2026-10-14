$ErrorActionPreference='Stop'
$path='C:/Users/Jerico/Documents/My Games/Hawken/HawkenGame/Logs/Launch.log'
$stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
try { $memory=[IO.MemoryStream]::new(); $stream.CopyTo($memory); $bytes=$memory.ToArray(); $memory.Dispose() } finally { $stream.Dispose() }
$text=[Text.Encoding]::GetEncoding(1252).GetString($bytes)
$lines=$text -split '\r?\n'
$seen=@{};$events=@()
$pattern='PlayerReplicationInfo|InitStormUserId|TickCriteria_UC|DeadWaitingStuff|SelectionStage|MechSelect|TeamSelect|GameClass|GameReplicationInfo|PlayerWaiting|ReceivedPlayer|myHUD|ShowMenu|CheckInGameMovie'
for($i=0;$i -lt $lines.Count;$i++){
 $line=$lines[$i]
 if($line -notmatch $pattern -or $line -notmatch 'ScriptWarning:|Warning:|ScriptLog:|InitStormUserId') {continue}
 if($line -match 'Authorization|AuthTicket|SteamTicket|accessToken|SessionTicket|Password|Bearer') {continue}
 $safe=$line -replace '(?i)[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}','[UUID]' -replace '(?i)\b[0-9a-f]{32}\b','[HEX32]' -replace '\b[0-9]{17}\b','[ID17]'
 $key=$safe -replace '^\[[^\]]*\]\s*',''
 $key=$key -replace '^\d{2}/\d{2}/\d{2}\s+\d{2}:\d{2}:\d{2}\s*',''
 if($seen.ContainsKey($key)){$seen[$key].count++;$seen[$key].last_line=$i+1;continue}
 $event=[pscustomobject]@{first_line=$i+1;last_line=$i+1;count=1;event=$safe}
 $seen[$key]=$event;$events+=$event
}
$sha=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLower()
$result=[ordered]@{source_path=$path;read_mode='FileStream FileShare.ReadWrite, full read into memory; no raw copy persisted';snapshot_sha256=$sha;snapshot_bytes=$bytes.Length;split_line_count=$lines.Count;encoding='Windows1252';selected_event_count=$events.Count;events=$events;limitations=@('Live file may continue growing/buffered','Absent selected log lines are not proof predicates did not execute','Only fixed public event classes selected; credentials excluded, GUID/hex32/17digit IDs redacted')}
$result|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $PSScriptRoot 'selected-events.json') -Encoding utf8
$result|ConvertTo-Json -Depth 6
