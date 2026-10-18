import hashlib
import json
import pathlib
import struct

target = pathlib.Path(r'F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe')
data = target.read_bytes()
digest = hashlib.sha256(data).hexdigest()
assert digest == '4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2'
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[pe:pe + 4] == b'PE\0\0'
assert struct.unpack_from('<H', data, pe + 4)[0] == 0x8664
opt = pe + 24
assert struct.unpack_from('<H', data, opt)[0] == 0x20b
base = struct.unpack_from('<Q', data, opt + 24)[0]
sections = []
table = opt + struct.unpack_from('<H', data, pe + 20)[0]
for index in range(struct.unpack_from('<H', data, pe + 6)[0]):
    entry = table + index * 40
    virtual_size, rva, size, raw = struct.unpack_from('<IIII', data, entry + 8)
    flags = struct.unpack_from('<I', data, entry + 36)[0]
    sections.append((rva, virtual_size, raw, size, flags))

def raw_rva(raw):
    return next(rva + raw - start for rva, _, start, size, _ in sections if start <= raw < start + size)

def rva_raw(rva):
    return next(start + rva - first for first, _, start, size, _ in sections if first <= rva < first + size)

def executable(rva):
    return any(first <= rva < first + size and flags & 0x20000000 for first, size, _, _, flags in sections)

exception_rva, exception_size = struct.unpack_from('<II', data, opt + 112 + 3 * 8)
exception_raw = rva_raw(exception_rva)
rows = []
for name in ('UObjectexecGetStateName', 'execGetStateName', 'GetStateName'):
    for encoding in ('ascii', 'utf-16-le'):
        needle = (name + '\0').encode(encoding)
        offset = 0
        while (hit := data.find(needle, offset)) >= 0:
            offset = hit + 1
            address = base + raw_rva(hit)
            pointer = struct.pack('<Q', address)
            scan = 0
            while (ref := data.find(pointer, scan)) >= 0:
                scan = ref + 1
                if ref + 16 > len(data):
                    continue
                function = struct.unpack_from('<Q', data, ref + 8)[0] - base
                if not executable(function):
                    continue
                bounds = []
                for item in range(exception_raw, exception_raw + exception_size, 12):
                    begin, end, unwind = struct.unpack_from('<III', data, item)
                    if begin <= function < end:
                        bounds.append({'begin_rva': hex(begin), 'end_rva': hex(end), 'unwind_rva': hex(unwind)})
                rows.append({'name': name, 'encoding': encoding, 'name_raw': hex(hit),
                             'table_raw': hex(ref), 'table_rva': hex(raw_rva(ref)),
                             'table_bytes': data[ref:ref + 16].hex(),
                             'candidate_exec_rva': hex(function), 'pdata': bounds})
anchors = []
for label, rva, size in (('execGetStateName', 0x5fde0, 0x98), ('fallback_literal', 0x12a5a38, 48)):
    raw = rva_raw(rva)
    chunk = data[raw:raw + size]
    anchors.append({'label': label, 'rva': hex(rva), 'raw': hex(raw), 'size': size,
                    'sha256': hashlib.sha256(chunk).hexdigest(), 'hex': chunk.hex()})
result = {'original': str(target), 'sha256': digest, 'image_base': hex(base), 'anchors': anchors,
          'executable_sections': [{'begin_rva': hex(rva), 'end_rva': hex(rva + size)}
                                  for rva, size, _, _, flags in sections if flags & 0x20000000],
          'candidates': rows, 'status': 'STATIC_REGISTRATION_CANDIDATES_ONLY'}
pathlib.Path(__file__).with_name('get-state-name-registration.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result))
