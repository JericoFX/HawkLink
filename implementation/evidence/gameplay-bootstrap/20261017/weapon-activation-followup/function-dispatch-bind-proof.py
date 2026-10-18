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
table = opt + struct.unpack_from('<H', data, pe + 20)[0]
sections = [struct.unpack_from('<IIII', data, table + i * 40 + 8)
            for i in range(struct.unpack_from('<H', data, pe + 6)[0])]
def raw(rva):
    return next(start + rva - first for _, first, size, start in sections if first <= rva < first + size)
exception_rva, exception_size = struct.unpack_from('<II', data, opt + 112 + 24)
rows = list(struct.iter_unpack('<III', data[raw(exception_rva):raw(exception_rva) + exception_size]))
anchors = []
for function in (0x6e540, 0x7bf30):
    found = [row for row in rows if row[0] == function]
    assert len(found) == 1
    begin, end, unwind = found[0]
    body = data[raw(begin):raw(begin) + end - begin]
    anchors.append({'rva': hex(begin), 'end_rva': hex(end), 'raw': hex(raw(begin)),
                    'size': len(body), 'sha256': hashlib.sha256(body).hexdigest()})
edges = []
for rva, expected in ((0x6e583, 'f786d000000000040000'),
                      (0x6e5a3, '0fb786d4000000'),
                      (0x6e5b2, '488d0de7c3b601'),
                      (0x6e829, '48899ef0000000'),
                      (0x6e832, '488d05f7d60000'),
                      (0x6e839, '488986f0000000')):
    value = data[raw(rva):raw(rva) + len(expected) // 2]
    assert value.hex() == expected
    edges.append({'rva': hex(rva), 'raw': hex(raw(rva)), 'bytes': value.hex()})
literal_start = raw(0x12b61e4)
literal_end = data.index(b'\0', literal_start)
result = {'schema': 'hawken.function-dispatch-binding-static-proof.v1',
          'original': str(target), 'sha256': digest, 'functions': anchors, 'edges': edges,
          'native_name_prefix_ascii': data[literal_start:literal_end].decode('ascii'),
          'static_interpretation': [
              'At6e583 receiver+d0 native bit400 clear selects6e832; RIP-relative LEA resolves7bf30 and stores it in receiver+f0 at6e839.',
              'Native bit400 set and associated object+124 bit4000 clear: token+d4 nonzero indexes QWORD table1bda9a0; token0 follows name lookup and stores result in+f0.',
              'CheckJumpOrHealOrTaunt retained flags20802 and ChangeCanFire20902 both have bit400 clear; if this binder processes those descriptors without later mutation, their expected target is7bf30.'],
          'status': 'STATIC_BINDER_BEHAVIOR_CONFIRMED;UFUNCTION_OWNER_AND_RUNTIME_BINDING_NOT_OBSERVED',
          'limits': ['No vtable/class ownership pin for binder6e540 yet.',
                     'No live+f0 read, callback execution or gameplay repair performed.']}
pathlib.Path(__file__).with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result))
