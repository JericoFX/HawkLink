$ErrorActionPreference = 'Stop'
$root = 'F:\HawkenServer'
$proofPath = Join-Path $root 'implementation\evidence\deathmatch-pawn-class\original-export-inspection-v2.json'
$proof = Get-Content -LiteralPath $proofPath -Raw | ConvertFrom-Json
$original = [IO.File]::ReadAllBytes($proof.source.path)
function Get-BytesHash([byte[]]$Bytes) {
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}
if ((Get-BytesHash $original) -cne $proof.source.sha256) { throw 'Original UPK hash mismatch.' }
$block = $proof.exports.'1'.source_zlib_blocks_for_serial[0]
$compressedOffset = [Convert]::ToInt32($block.physical_compressed_offset, 16)
$virtualStart = [Convert]::ToInt32($block.virtual_start, 16)
$input = [IO.MemoryStream]::new($original, $compressedOffset, [int]$block.compressed_size, $false)
$zlib = [IO.Compression.ZLibStream]::new($input, [IO.Compression.CompressionMode]::Decompress)
$output = [IO.MemoryStream]::new()
try { $zlib.CopyTo($output); $plain = $output.ToArray() }
finally { $zlib.Dispose(); $input.Dispose(); $output.Dispose() }
if ($plain.Length -ne $block.plain_size -or (Get-BytesHash $plain) -cne $block.plain_sha256) {
    throw 'Original compressed block mismatch.'
}
$rows = foreach ($index in @('1', '19')) {
    $expected = $proof.exports.$index
    $serialStart = [Convert]::ToInt32($expected.serial_virtual_range[0], 16) - $virtualStart
    $recordStart = [Convert]::ToInt32($expected.record_virtual_range[0], 16) - $virtualStart
    $serial = [byte[]]$plain[$serialStart..($serialStart + $expected.serial_length - 1)]
    $record = [byte[]]$plain[$recordStart..($recordStart + $expected.record_length - 1)]
    if ((Get-BytesHash $serial) -cne $expected.serial_sha256 -or
        (Get-BytesHash $record) -cne $expected.record_sha256) { throw 'Export bytes mismatch.' }
    if ([BitConverter]::ToInt32($record, 0) -ne 0 -or
        [BitConverter]::ToInt32($record, 4) -ne $expected.fields.super_ref.index) {
        throw 'Class export or superclass mismatch.'
    }
    $parentIndex = $expected.fields.super_ref.index
    $parentOffset = [Convert]::ToInt32($proof.summary.import_table_virtual_offset, 16) +
        ((-$parentIndex - 1) * 28) - $virtualStart
    $parentRecord = [byte[]]$plain[$parentOffset..($parentOffset + 27)]
    if ([Convert]::ToHexString($parentRecord).ToLowerInvariant() -cne
        $expected.fields.super_ref.chain_from_inner_to_outer[0].raw_record) {
        throw 'Superclass import bytes mismatch.'
    }
    [ordered]@{
        physical_export = [int]$index
        class_name = $expected.fields.object_name
        parent_name = $expected.fields.super_ref.chain_from_inner_to_outer[0].object_name
        serial_offset = $expected.serial_virtual_range[0]
        serial_sha256 = Get-BytesHash $serial
        record_sha256 = Get-BytesHash $record
        local_net_index = [BitConverter]::ToInt32($serial, 0)
    }
}
if ($rows[0].class_name -cne 'HawkenDM' -or $rows[0].parent_name -cne 'R_Deathmatch' -or
    $rows[0].local_net_index -ne 105 -or $rows[1].local_net_index -ne 232) {
    throw 'Corrected metadata does not match original class evidence.'
}
$main = Get-Content -LiteralPath (Join-Path $root 'src\server_main.cpp') -Raw
if ($main -notmatch '\{0x5cd139c7U,0x4aa73e14U,0xbd1c8593U,0xf86056ddU\},105,') {
    throw 'Configured GameClass does not select the original HawkenDM NetIndex.'
}
$result = [ordered]@{
    status = 'ORIGINAL_BYTES_AND_CONFIGURED_GAMECLASS_VERIFIED'
    original_path = $proof.source.path
    original_sha256 = Get-BytesHash $original
    retained_proof_sha256 = (Get-FileHash -LiteralPath $proofPath -Algorithm SHA256).Hash.ToLowerInvariant()
    block_plain_sha256 = Get-BytesHash $plain
    exports = @($rows)
    corrected_package = 'HawkenGameContent'
    corrected_local_net_index = 105
    conditional_primary_base = 104786
    conditional_coordinate = 104891
    main_sha256 = (Get-FileHash -LiteralPath (Join-Path $root 'src\server_main.cpp') -Algorithm SHA256).Hash.ToLowerInvariant()
    native_corrected_class_observed = $false
    limitation = 'Class names use retained hash-bound export/name metadata; serial, class/super export and parent import bytes were reread from original compressed UPK. Native acceptance remains pending.'
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'original-verification.json') -Encoding utf8
$result | ConvertTo-Json -Depth 8
