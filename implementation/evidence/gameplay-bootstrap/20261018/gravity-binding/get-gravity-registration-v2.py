"""Bounded literal recovery and registration candidates; no target execution."""
import hashlib
import json
import pathlib
import struct

target = pathlib.Path(r'F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe')
source = pathlib.Path(__file__)
output = source.with_name('get-gravity-registration-v2.json')
if output.exists():
    raise RuntimeError('Retained output exists; do not overwrite prior evidence')
data = target.read_bytes()
digest = hashlib.sha256(data).hexdigest()
if digest != '4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2':
    raise RuntimeError('Original identity mismatch')
pe = struct.unpack_from('<I', data, 0x3c)[0]
if data[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', data, pe + 4)[0] != 0x8664:
    raise RuntimeError('Expected original AMD64 PE')
opt = pe + 24
if struct.unpack_from('<H', data, opt)[0] != 0x20b:
    raise RuntimeError('Expected original PE32+')
base = struct.unpack_from('<Q', data, opt + 24)[0]
sections = []
table = opt + struct.unpack_from('<H', data, pe + 20)[0]
for index in range(struct.unpack_from('<H', data, pe + 6)[0]):
    entry = table + index * 40
    virtual_size, rva, size, raw = struct.unpack_from('<IIII', data, entry + 8)
    flags = struct.unpack_from('<I', data, entry + 36)[0]
    if raw + size > len(data):
        raise RuntimeError('Section raw span exceeds original file')
    sections.append((rva, virtual_size, raw, size, flags))


def raw_rva(raw, length=1):
    return next(rva + raw - start for rva, _, start, size, _ in sections
                if start <= raw and raw + length <= start + size)


def rva_raw(rva, length=1):
    return next(start + rva - first for first, _, start, size, _ in sections
                if first <= rva and rva + length <= first + size)


def executable(rva):
    return any(first <= rva < first + size and flags & 0x20000000
               for first, size, _, _, flags in sections)


exception_rva, exception_size = struct.unpack_from('<II', data, opt + 112 + 3 * 8)
if not exception_rva or exception_size % 12:
    raise RuntimeError('Invalid exception directory')
exception_raw = rva_raw(exception_rva, exception_size)


def pdata(function):
    ranges = []
    for item in range(exception_raw, exception_raw + exception_size, 12):
        begin, end, unwind = struct.unpack_from('<III', data, item)
        if begin <= function < end:
            ranges.append({'begin_rva': hex(begin), 'end_rva': hex(end),
                           'begin_raw': hex(rva_raw(begin, end - begin)), 'size': end - begin,
                           'unwind_rva': hex(unwind), 'pdata_raw': hex(item),
                           'pdata_bytes': data[item:item + 12].hex(),
                           'function_is_range_entry': function == begin})
    return ranges


explicit_variants = ('UActorexecGetGravityZ', 'UActor::execGetGravityZ', 'execGetGravityZ', 'GetGravityZ')
control_name = 'UObjectexecGetStateName'
matches = []
candidates = []
for purpose, suffix in (('gravity', 'GetGravityZ'), ('control', control_name)):
    for encoding, unit in (('ascii', 1), ('utf-16-le', 2)):
        needle = suffix.encode(encoding)
        nul = bytes(unit)
        offset = 0
        while (hit := data.find(needle, offset)) >= 0:
            offset = hit + 1
            match = {'purpose': purpose, 'encoding': encoding, 'suffix_hit_raw': hex(hit), 'refs64': []}
            matches.append(match)
            section = next((item for item in sections
                            if item[2] <= hit and hit + len(needle) <= item[2] + item[3]), None)
            if section is None:
                match['rejection'] = 'suffix_outside_section'
                continue
            match['suffix_hit_rva'] = hex(raw_rva(hit, len(needle)))
            end = hit + len(needle)
            if end + unit > section[2] + section[3] or data[end:end + unit] != nul:
                match['rejection'] = 'suffix_not_literal_end'
                continue
            start = hit
            while start - unit >= section[2] and data[start - unit:start] != nul:
                if (end - start) // unit >= 128:
                    break
                start -= unit
            if (end - start) // unit > 128 or (
                    start - unit >= section[2] and data[start - unit:start] != nul):
                match['rejection'] = 'literal_exceeds_128_units'
                continue
            literal = data[start:end + unit]
            match.update({'name_raw': hex(start), 'name_rva': hex(raw_rva(start, len(literal))),
                          'name_size': len(literal), 'name_sha256': hashlib.sha256(literal).hexdigest(),
                          'suffix_offset_bytes': hit - start,
                          'literal_boundary': 'nul' if start - unit >= section[2] else 'section_start'})
            try:
                text = data[start:end].decode(encoding, errors='strict')
            except UnicodeError:
                match['rejection'] = 'invalid_literal_encoding'
                continue
            if any(ord(character) < 0x20 or ord(character) == 0x7f for character in text):
                match['rejection'] = 'nonprintable_literal'
                continue
            match.update({'name': text, 'explicit_variant': text in explicit_variants})
            pointer = struct.pack('<Q', base + raw_rva(start))
            scan = 0
            while (ref := data.find(pointer, scan)) >= 0:
                scan = ref + 1
                reference = {'raw': hex(ref)}
                match['refs64'].append(reference)
                try:
                    reference['rva'] = hex(raw_rva(ref, 16))
                except StopIteration:
                    reference['rejection'] = 'pair_outside_section'
                    continue
                pair = data[ref:ref + 16]
                function = struct.unpack_from('<Q', pair, 8)[0] - base
                reference.update({'bytes': pair.hex(), 'sha256': hashlib.sha256(pair).hexdigest(),
                                  'following_target_rva': hex(function)})
                if not executable(function):
                    reference['rejection'] = 'following_target_not_executable'
                    continue
                candidate = {'purpose': purpose, 'name': text, 'encoding': encoding,
                             'name_raw': hex(start), 'name_rva': match['name_rva'],
                             'name_sha256': match['name_sha256'], 'suffix_hit_raw': hex(hit),
                             'table_raw': hex(ref), 'table_rva': reference['rva'],
                             'table_bytes': pair.hex(), 'table_sha256': reference['sha256'],
                             'candidate_exec_rva': hex(function), 'candidate_exec_raw': hex(rva_raw(function)),
                             'pdata': pdata(function)}
                candidates.append(candidate)
                reference['candidate_index'] = len(candidates) - 1

# Expected control values are comparisons only: all reported candidates come
# from the scans above. A missing control prevents a successful search status.
control_reproduced = any(
    row['purpose'] == 'control' and row['name'] == control_name and row['encoding'] == 'ascii'
    and row['name_raw'] == '0x12b3420' and row['table_raw'] == '0x1add5a0'
    and row['table_rva'] == '0x1ade1a0' and row['candidate_exec_rva'] == '0x5fde0'
    and any(span['begin_rva'] == '0x5fde0' and span['end_rva'] == '0x5fe78' for span in row['pdata'])
    for row in candidates)
result = {'original': str(target), 'sha256': digest, 'image_base': hex(base),
          'script': str(source.resolve()), 'script_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'literal_limit_units': 128, 'matches': matches, 'candidates': candidates,
          'control_reproduced': control_reproduced,
          'status': 'STATIC_REGISTRATION_CANDIDATES_ONLY' if control_reproduced else 'INCONCLUSIVE_CONTROL_NOT_REPRODUCED',
          'exception_directory': {'rva': hex(exception_rva), 'raw': hex(exception_raw), 'size': exception_size},
          'limitations': ['No target execution, body decompilation, gravity formula or virtual-slot inference.',
                          'Pointer pairs and containing pdata ranges are candidates, not complete wrapper bindings.']}
with output.open('x', encoding='utf-8') as evidence:
    evidence.write(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
