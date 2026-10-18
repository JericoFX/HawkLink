param([string]$InstructionsPath,[string]$OutputPath)
$ErrorActionPreference='Stop'
$root='F:\HawkenServer'
$original='F:\\SteamLibrary\\steamapps\\common\\Hawken\\Binaries\\Win64\\HawkenGame-Win64-Shipping.exe'
$expectedSha='4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2'
$entry='1403fd720'
$expectedLength=76
$expectedRawOffset=0x3fcb20
$baselinePath=Join-Path $PSScriptRoot 'original-range-baseline.json'
$baseline=Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
if($baseline.originalProgramSha256 -ne $expectedSha -or $baseline.lengthBytes -ne $expectedLength -or $baseline.rawOffset -ne '0x3fcb20'){throw 'Pinned original range baseline metadata mismatch'}
function Normalize-Address([string]$Value){($Value -replace '^0x','').ToLowerInvariant()}
if((Get-FileHash -LiteralPath $original -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedSha){throw 'Original PE SHA mismatch'}
$dump=Get-Content -LiteralPath $InstructionsPath -Raw | ConvertFrom-Json
if($dump.program_sha256.ToLowerInvariant() -ne $expectedSha){throw 'Instruction dump program SHA mismatch'}
$matches=@($dump.functions | Where-Object {(Normalize-Address $_.entry) -eq $entry})
if($matches.Count -ne 1){throw 'Expected exactly one function entry'}
$rows=@($matches[0].instructions)
if($rows.Count -eq 0){throw 'No instructions in function body'}
[long]$next=[Convert]::ToInt64($entry,16)
$allHex=[System.Text.StringBuilder]::new()
$addressErrors=[Collections.Generic.List[object]]::new()
foreach($row in $rows){
    $address=(Normalize-Address ([string]$row.address))
    $hex=([string]$row.bytes).ToLowerInvariant()
    if($hex.Length -eq 0 -or ($hex.Length % 2) -ne 0 -or $hex -notmatch '^[0-9a-f]+$'){throw ('Invalid instruction byte string at '+$row.address)}
    [long]$actual=[Convert]::ToInt64($address,16)
    if($actual -ne $next){$addressErrors.Add(@{expected=('0x{0:x}' -f $next);actual=('0x{0:x}' -f $actual)})}
    [void]$allHex.Append($hex)
    $next=$actual+($hex.Length/2)
}
$joined=$allHex.ToString()
$totalBytes=$joined.Length/2
if($totalBytes -ne $expectedLength){throw ('Expected 76 instruction bytes; got '+$totalBytes)}
$decoded=New-Object byte[] $totalBytes
for($i=0;$i -lt $totalBytes;$i++){$decoded[$i]=[Convert]::ToByte($joined.Substring($i*2,2),16)}
$stream=[System.IO.File]::OpenRead($original)
try{$stream.Position=$expectedRawOffset;$raw=New-Object byte[] $expectedLength;$read=$stream.Read($raw,0,$raw.Length)}finally{$stream.Dispose()}
if($read -ne $expectedLength){throw 'Could not read complete original range'}
$baselineHex=([string]$baseline.rawBytesHex).ToLowerInvariant()
if($baselineHex.Length -ne ($expectedLength*2)){throw 'Pinned original range baseline byte count mismatch'}
$mismatches=[Collections.Generic.List[object]]::new()
for($i=0;$i -lt $expectedLength;$i++){if($decoded[$i] -ne $raw[$i]){$mismatches.Add(@{offset=$i;va=('0x{0:x}' -f ([Convert]::ToInt64($entry,16)+$i));instructionByte=('{0:x2}' -f $decoded[$i]);originalByte=('{0:x2}' -f $raw[$i])})}}
$record=[ordered]@{originalSha256=$expectedSha;entry=('0x'+$entry);rangeEnd=('0x{0:x}' -f $next);expectedFunctionBytes=$expectedLength;instructionCount=$rows.Count;instructionBytesSha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($decoded)).ToLowerInvariant();originalBytesSha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($raw)).ToLowerInvariant();baselineBytesAgree=($baselineHex -eq ([Convert]::ToHexString($raw).ToLowerInvariant()));addressDiscontinuities=$addressErrors.ToArray();byteMismatches=$mismatches.ToArray();contiguous=($addressErrors.Count -eq 0 -and $next -eq ([Convert]::ToInt64($entry,16)+$expectedLength));bytesMatch=($mismatches.Count -eq 0);rawOffset=('0x{0:x}' -f $expectedRawOffset);verifiedUtc=[DateTime]::UtcNow.ToString('o')}
$record | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $OutputPath -Encoding utf8

if(-not $record.contiguous -or -not $record.bytesMatch -or -not $record.baselineBytesAgree){exit 2}
