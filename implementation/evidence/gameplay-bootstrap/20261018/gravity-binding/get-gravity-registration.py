"""Static registration candidates only; never executes or changes the target PE."""
import hashlib
import json
import pathlib
import struct

target = pathlib.Path(r'F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe')
source = pathlib.Path(__file__)
output = source.with_name('get-gravity-registration.json')
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
rows = []
# Concatenated class/exec is the retained native registration naming pattern;
# the scoped and unqualified literals remain candidates, not identity proof.
for name in ('UActorexecGetGravityZ', 'UActor::execGetGravityZ', 'execGetGravityZ', 'GetGravityZ'):
    for encoding in ('ascii', 'utf-16-le'):
        needle = (name + '\0').encode(encoding)
        offset = 0
        while (hit := data.find(needle, offset)) >= 0:
            offset = hit + 1
            try:
                name_rva = raw_rva(hit, len(needle))
            except StopIteration:
                continue
            pointer = struct.pack('<Q', base + name_rva)
            scan = 0
            while (ref := data.find(pointer, scan)) >= 0:
                scan = ref + 1
                try:
                    table_rva = raw_rva(ref, 16)
                except StopIteration:
                    continue
                function_va = struct.unpack_from('<Q', data, ref + 8)[0]
                function = function_va - base
                if not executable(function):
                    continue
                function_raw = rva_raw(function)
                bounds = []
                for item in range(exception_raw, exception_raw + exception_size, 12):
                    begin, end, unwind = struct.unpack_from('<III', data, item)
                    if begin <= function < end:
                        body_raw = rva_raw(begin, end - begin)
                        bounds.append({'begin_rva': hex(begin), 'end_rva': hex(end),
                                       'begin_raw': hex(body_raw), 'size': end - begin,
                                       'unwind_rva': hex(unwind), 'pdata_raw': hex(item),
                                       'pdata_bytes': data[item:item + 12].hex(),
                                       'function_is_range_entry': function == begin})
                pair = data[ref:ref + 16]
                rows.append({'name': name, 'encoding': encoding, 'name_raw': hex(hit),
                             'name_rva': hex(name_rva), 'name_size': len(needle),
                             'name_sha256': hashlib.sha256(needle).hexdigest(),
                             'table_raw': hex(ref), 'table_rva': hex(table_rva),
                             'table_bytes': pair.hex(),
                             'table_sha256': hashlib.sha256(pair).hexdigest(),
                             'candidate_exec_rva': hex(function),
                             'candidate_exec_raw': hex(function_raw), 'pdata': bounds})
result = {'original': str(target), 'sha256': digest, 'image_base': hex(base),
          'script': str(source.resolve()), 'script_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'executable_sections': [{'begin_rva': hex(rva), 'end_rva': hex(rva + size)}
                                  for rva, size, _, _, flags in sections if flags & 0x20000000],
          'exception_directory': {'rva': hex(exception_rva), 'raw': hex(exception_raw), 'size': exception_size},
          'candidates': rows, 'status': 'STATIC_REGISTRATION_CANDIDATES_ONLY',
          'limitations': ['No function-body decompilation or target execution.',
                          'A pointer pair and containing pdata range do not prove a wrapper or virtual callee.',
                          'No gravity formula, virtual slot or Pawn target is inferred.']}
with output.open('x', encoding='utf-8') as evidence:
    evidence.write(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
