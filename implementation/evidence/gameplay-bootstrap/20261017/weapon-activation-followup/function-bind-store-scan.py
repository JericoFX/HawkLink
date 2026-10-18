"""Bounded static candidates for the known UFunction+f0 dispatch slot."""
import bisect
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
opt = pe + 24
assert struct.unpack_from('<H', data, opt)[0] == 0x20b
table = opt + struct.unpack_from('<H', data, pe + 20)[0]
sections = [struct.unpack_from('<IIII', data, table + i * 40 + 8)
            for i in range(struct.unpack_from('<H', data, pe + 6)[0])]

def raw(rva):
    return next(start + rva - first for _, first, size, start in sections
                if first <= rva < first + size)

exception_rva, exception_size = struct.unpack_from('<II', data, opt + 112 + 24)
rows = list(struct.iter_unpack('<III', data[raw(exception_rva):raw(exception_rva) + exception_size]))
starts = [row[0] for row in rows]
assert starts == sorted(starts)
text_first, text_last = 0x1000, 0x128df79
first, last = raw(text_first), raw(text_last - 1) + 1
hits = []
needle = struct.pack('<I', 0xf0)
position = first
while (offset := data.find(needle, position, last)) >= 0:
    position = offset + 1
    if offset < first + 3:
        continue
    rex, opcode, modrm = data[offset - 3:offset]
    # REX.W MOV r64 -> [base+disp32], excluding SIB and RIP-relative forms.
    if rex & 0xf8 != 0x48 or opcode != 0x89 or modrm & 0xc0 != 0x80 or modrm & 7 == 4:
        continue
    rva = text_first + offset - first - 3
    index = bisect.bisect_right(starts, rva) - 1
    if index < 0 or not rows[index][0] <= rva < rows[index][1]:
        continue
    begin, end, unwind = rows[index]
    body = data[raw(begin):raw(begin) + end - begin]
    hits.append({'store_rva': hex(rva), 'function_rva': hex(begin), 'function_end': hex(end),
                 'function_bytes': len(body), 'body_sha256': hashlib.sha256(body).hexdigest(),
                 'instruction_prefix': data[offset - 3:offset + 4].hex(),
                 'has_d4_displacement_bytes': struct.pack('<I', 0xd4) in body,
                 'has_d0_or_d1_displacement_bytes': any(struct.pack('<I', n) in body for n in (0xd0, 0xd1))})
selected = [hit for hit in hits if hit['function_bytes'] <= 4096 and
            hit['has_d4_displacement_bytes'] and hit['has_d0_or_d1_displacement_bytes']]
result = {'schema': 'hawken.function-dispatch-store-candidates.v1', 'original': str(target),
          'sha256': digest, 'scope': 'Known .text range, REX.W MOV register-to-base+f0 only',
          'stores_with_pdata': len(hits), 'cooccurrence_candidates': selected,
          'all_store_candidates': hits, 'status': 'STATIC_BYTE_CANDIDATES_ONLY',
          'limits': ['Byte cooccurrences do not establish decoded operands, object type or UFunction ownership.',
                     'Excludes SIB, immediate stores, other instructions and functions without pdata.',
                     'No current callback pointer, live call, target execution or gameplay result observed.']}
out = pathlib.Path(__file__).with_suffix('.json')
out.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps({key: value for key, value in result.items() if key != 'all_store_candidates'}))
