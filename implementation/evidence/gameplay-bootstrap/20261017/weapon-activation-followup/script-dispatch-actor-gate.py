import hashlib
import json
import pathlib
import struct

target = pathlib.Path(r'F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe')
data = target.read_bytes()
digest = hashlib.sha256(data).hexdigest()
assert digest == '4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2'
pe = struct.unpack_from('<I', data, 0x3c)[0]
opt = pe + 24
base = struct.unpack_from('<Q', data, opt + 24)[0]
table = opt + struct.unpack_from('<H', data, pe + 20)[0]
sections = [struct.unpack_from('<IIII', data, table + i * 40 + 8)
            for i in range(struct.unpack_from('<H', data, pe + 6)[0])]
def raw(rva):
    return next(start + rva - first for _, first, size, start in sections if first <= rva < first + size)
exception_rva, exception_size = struct.unpack_from('<II', data, opt + 112 + 24)
rows = list(struct.iter_unpack('<III', data[raw(exception_rva):raw(exception_rva) + exception_size]))
vtable = 0x17de0c0
entries = []
for slot in (0x240, 0x258, 0x408):
    pointer = struct.unpack_from('<Q', data, raw(vtable + slot))[0]
    rva = pointer - base
    bounds = [row for row in rows if row[0] == rva]
    assert len(bounds) == 1
    begin, end, unwind = bounds[0]
    body = data[raw(begin):raw(begin) + end - begin]
    entries.append({'slot': hex(slot), 'entry_rva': hex(vtable + slot),
                    'entry_raw': hex(raw(vtable + slot)), 'entry_bytes': struct.pack('<Q', pointer).hex(),
                    'target_rva': hex(rva), 'end_rva': hex(end), 'size': len(body),
                    'body_sha256': hashlib.sha256(body).hexdigest()})
result = {'schema': 'hawken.script-dispatch-actor-vtable-gate.v1', 'original': str(target),
          'sha256': digest, 'vtable_rva': hex(vtable), 'entries': entries,
          'scope': 'Retained Pawn-native vtable seed; exact static entries only',
          'status': 'CONFIRMED_ENTRY_BYTES;RUNTIME_VTABLE_AND_GATES_NOT_OBSERVED'}
pathlib.Path(__file__).with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result))
