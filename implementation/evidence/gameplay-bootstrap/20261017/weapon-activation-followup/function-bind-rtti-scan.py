import hashlib
import json
import mmap
import pathlib
import struct

path = pathlib.Path(r'F:\SteamLibrary\steamapps\common\Hawken\Binaries\Win64\HawkenGame-Win64-Shipping.exe')
expected = '4a861ecb4ac7add9554f8daf8f98ad2c12b9cb6c1865a83e8be808beb46ea8a2'
with path.open('rb') as f:
    size = path.stat().st_size
    digest = hashlib.sha256()
    while chunk := f.read(1024 * 1024):
        digest.update(chunk)
    sha = digest.hexdigest()
    if sha != expected:
        raise SystemExit(f'original hash mismatch: {sha}')
    f.seek(0)
    with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        if data[pe:pe + 4] != b'PE\0\0':
            raise SystemExit('missing PE signature')
        machine = struct.unpack_from('<H', data, pe + 4)[0]
        optional = pe + 24
        magic = struct.unpack_from('<H', data, optional)[0]
        if machine != 0x8664 or magic != 0x20b:
            raise SystemExit(f'unexpected PE identity: machine={machine:#x}, optional={magic:#x}')
        image_base = struct.unpack_from('<Q', data, optional + 24)[0]
        section_table = optional + struct.unpack_from('<H', data, pe + 20)[0]
        sections = []
        for i in range(struct.unpack_from('<H', data, pe + 6)[0]):
            p = section_table + 40 * i
            name = bytes(data[p:p + 8]).split(b'\0', 1)[0].decode('ascii', 'replace')
            virtual_size, rva, raw_size, raw = struct.unpack_from('<IIII', data, p + 8)
            sections.append({'name': name, 'rva': rva, 'virtual_size': virtual_size,
                             'raw_offset': raw, 'raw_size': raw_size})
        def raw_to_rva(offset):
            for sec in sections:
                if sec['raw_offset'] <= offset < sec['raw_offset'] + sec['raw_size']:
                    return sec['rva'] + offset - sec['raw_offset']
            return None
        needle = b'.?AVUFunction@@'
        hits = []
        start = 0
        while True:
            pos = data.find(needle, start)
            if pos < 0:
                break
            hits.append({'raw_offset': pos, 'rva': raw_to_rva(pos), 'va': image_base + raw_to_rva(pos)
                         if raw_to_rva(pos) is not None else None})
            start = pos + 1
        result = {
            'scope': 'Exact ASCII MSVC class RTTI name scan only; no broad reference scan or method disassembly after zero hits.',
            'original': str(path), 'sha256': sha, 'bytes': size,
            'pe': {'machine': hex(machine), 'optional_magic': hex(magic), 'image_base': hex(image_base),
                   'sections': sections},
            'needle': needle.decode('ascii'), 'matches': hits, 'match_count': len(hits),
            'complete_object_locator_scan_performed': False,
            'vtable_or_method_candidates_scanned': False,
            'conclusion': 'NO_EXACT_MSVCTYPEDESCRIPTOR_STRING_FOUND' if not hits else 'RTTI_STRING_FOUND_REQUIRES_BOUNDED_COL_CHECK'
        }
        out = path.parent
        target = pathlib.Path(r'F:\HawkenServer\implementation\evidence\gameplay-bootstrap\20261017\weapon-activation-followup\function-bind-rtti-scan.json')
        target.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({'output': str(target), 'result': result}, indent=2))
