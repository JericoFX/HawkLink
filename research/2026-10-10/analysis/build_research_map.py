"""Index retained Hawken evidence; never execute, modify or decompile the game.

The graph is an index of observations, not a claim that every function was
recovered or that an imported module/call path executed at runtime.
"""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write_csv(path, rows, fields):
    with path.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def build(root, output):
    research = root / 'research/2026-10-10'
    evidence = research / 'evidence'
    inputs = {}

    def pin(path):
        relative = path.relative_to(root).as_posix()
        inputs[relative] = digest(path)
        return relative

    inventory_path = evidence / 'inventory.csv'
    pin(inventory_path)
    with inventory_path.open(encoding='utf-8-sig', newline='') as stream:
        inventory = list(csv.DictReader(stream))
    original_root = Path(inventory[0]['AbsolutePath'])
    for part in Path(inventory[0]['RelativePath']).parts:
        original_root = original_root.parent
    if output.is_relative_to(original_root.resolve()):
        raise ValueError('Research output must be outside the original game tree')
    output.mkdir(parents=True, exist_ok=True)
    # Bind identical files at multiple paths without selecting an arbitrary one.
    by_hash = collections.defaultdict(list)
    for row in inventory:
        by_hash[row['SHA256'].lower()].append(row)

    pe_path = evidence / 'pe-linkage.json'
    pin(pe_path)
    pe = load(pe_path)
    if pe['errors']:
        raise ValueError('Retained PE parser contains errors')
    records = pe['records']
    modules = collections.defaultdict(list)
    image_bases = {}
    for row in records:
        image_bases[row['sha256'].lower()] = int(row['image_base'], 16)
        modules[Path(row['relative_path'].replace('\\', '/')).name.lower()].append(row)
        matches = by_hash[row['sha256'].lower()]
        if not any(m['RelativePath'] == row['relative_path'] for m in matches):
            raise ValueError('PE path/hash not bound to inventoried original')
    linkage = []
    for row in records:
        for kind, field in [('normal', 'imports'), ('delay', 'delay_imports')]:
            for descriptor in row[field]:
                module = descriptor['module']
                targets = modules.get(module.lower(), [])
                linkage.append({
                    'source': row['relative_path'], 'source_sha256': row['sha256'],
                    'kind': kind, 'module': module,
                    'descriptor_rva': descriptor['descriptor_rva'],
                    'symbol_count': len(descriptor['functions']),
                    'bundled_candidates': json.dumps([
                        {'path': t['relative_path'], 'sha256': t['sha256']}
                        for t in targets], ensure_ascii=False),
                    'state': 'CONFIRMADO_DESCRIPTOR;NO_VERIFICADO_CARGADOR',
                })

    functions = {}
    occurrences = []
    calls = {}
    failures = []
    parse_errors = []
    non_json_logs = []
    examined = 0
    accepted_files = 0
    for directory in [evidence, root / 'implementation/evidence']:
        for path in sorted(directory.rglob('*.jsonl')):
            if output == path.parent or output in path.parents:
                continue
            examined += 1
            retained = []
            file_errors = []
            file_hash_before = digest(path)
            with path.open(encoding='utf-8-sig') as stream:
                for line_number, line in enumerate(stream, 1):
                    try:
                        row = json.loads(line)
                    except (ValueError, TypeError) as error:
                        file_errors.append({'path': path.relative_to(root).as_posix(),
                                            'line': line_number, 'error': str(error)})
                        continue
                    if isinstance(row, dict) and 'pseudocode' in row:
                        retained.append((line_number, row))
            if not retained:
                non_json_logs.extend(file_errors)
                continue
            parse_errors.extend(file_errors)
            accepted_files += 1
            source = pin(path)
            if inputs[source] != file_hash_before:
                raise ValueError('Dossier changed during indexing: ' + source)
            for line_number, row in retained:
                sha = str(row.get('program_sha256', '')).lower()
                if sha not in by_hash:
                    failures.append({'source': source, 'line': line_number,
                                     'reason': 'Unknown original program SHA-256'})
                    continue
                if not row.get('decompile_completed') or not row['pseudocode'].strip():
                    failures.append({'source': source, 'line': line_number,
                                     'reason': 'No completed pseudocode dossier'})
                    continue
                try:
                    address = int(row['entry'], 16)
                    base = (int(row['image_base'], 16) if 'image_base' in row
                            else image_bases[sha])
                except (KeyError, ValueError, TypeError):
                    failures.append({'source': source, 'line': line_number,
                                     'reason': 'Missing or invalid entry/image base'})
                    continue
                if address < base:
                    raise ValueError('Native entry precedes its image base')
                key = (sha, address)
                node = functions.setdefault(key, {
                    'original_sha256': sha, 'original_paths': sorted(
                        r['RelativePath'] for r in by_hash[sha]),
                    'entry_va': hex(address), 'rva': hex(address - base),
                    'names': set(), 'pseudocode_variants': set(), 'sources': [],
                    'state': 'CONFIRMADO_DOSSIER_ESTATICO;NO_VERIFICADO_RUNTIME',
                })
                node['names'].add(row.get('function', ''))
                pseudo_hash = hashlib.sha256(row['pseudocode'].encode('utf-8')).hexdigest()
                node['pseudocode_variants'].add(pseudo_hash)
                location = {'source': source, 'source_sha256': inputs[source],
                            'line': line_number, 'pseudocode_sha256': pseudo_hash,
                            'evidence_id': row.get('evidence_id')}
                node['sources'].append(location)
                occurrences.append({'original_sha256': sha, 'entry_va': hex(address),
                                    'rva': hex(address - base), **location})
                # Only provider's explicit callee list; do not infer virtual
                # dispatch targets or turn pseudocode mentions into call edges.
                for callee in row.get('callees', []):
                    try:
                        target = int(callee['entry'], 16)
                    except (KeyError, ValueError, TypeError):
                        continue
                    if target < base:
                        continue
                    edge = calls.setdefault((sha, address, target), {
                        'original_sha256': sha, 'from_va': hex(address),
                        'from_rva': hex(address - base), 'to_va': hex(target),
                        'to_rva': hex(target - base), 'sources': [],
                        'state': 'CONFIRMADO_REFERENCIA_PROVIDER;NO_VERIFICADO_RUNTIME',
                    })
                    edge['sources'].append({'source': source, 'line': line_number})

    nodes = []
    for key, row in sorted(functions.items()):
        row['names'] = sorted(row['names'])
        row['pseudocode_variants'] = sorted(row['pseudocode_variants'])
        nodes.append(row)
    edges = []
    for key, row in sorted(calls.items()):
        row['target_has_dossier'] = (key[0], key[2]) in functions
        edges.append(row)

    report_index = []
    report_names = [
        'HAWKEN_BINARY_INVENTORY.md', 'HAWKEN_ENDPOINT_CATALOG.md',
        'HAWKEN_AUTH_PROTOCOL.md', 'HAWKEN_MASTER_SERVER_PROTOCOL.md',
        'HAWKEN_GAME_SERVER_PROTOCOL.md', 'HAWKEN_NETWORK_ARCHITECTURE.md',
        'HAWKEN_UNKNOWN_AND_BLOCKERS.md',
    ]
    for name in report_names:
        path = research / name
        relative = pin(path)
        for line_number, text in enumerate(path.read_text(encoding='utf-8-sig').splitlines(), 1):
            if text.startswith('#'):
                report_index.append({'report': relative, 'line': line_number,
                                     'heading': text.lstrip('#').strip(),
                                     'report_sha256': inputs[relative]})

    summary = {
        'scope': 'All inventoried PE linkage and retained top-level JSONL native dossiers '
                 'under research/evidence and implementation/evidence; no fresh decompilation.',
        'inventory_files': len(inventory), 'pe_files': len(records),
        'pe_linkage_descriptors': len(linkage), 'jsonl_files_examined': examined,
        'jsonl_files_with_pseudocode': accepted_files,
        'native_dossier_occurrences': len(occurrences),
        'native_unique_sha_entry': len(nodes),
        'native_by_original': dict(collections.Counter(n['original_sha256'] for n in nodes)),
        'unique_provider_call_edges': len(edges),
        'callee_frontier_without_dossier': sum(not e['target_has_dossier'] for e in edges),
        'retained_failure_records': failures, 'jsonl_parse_errors': parse_errors,
        'non_dossier_logs_with_jsonl_suffix': non_json_logs,
        'report_sections': len(report_index),
        'inputs': [{'path': path, 'sha256': sha} for path, sha in sorted(inputs.items())],
        'limits': [
            'Index coverage is not full semantic recovery or original source.',
            'Repeated dossiers include retained copies; occurrences are not analysis runs.',
            'JSON-only/native-instruction evidence without top-level pseudocode is outside '
            'the native-function count; remains in the reports and prior evidence.',
            'Missing dossier image bases are bound from PE metadata by original SHA-256. '
            'Non-JSON logs with a JSONL suffix are retained separately; malformed lines '
            'in a file containing pseudocode dossiers fail acceptance.',
            'Bundled module basename matches are candidates, not observed loader resolution.',
            'Provider call edges do not close indirect/virtual calls or demonstrate execution.',
            'UPK coverage is provided by the separate package map; native declarations are '
            'not implementations, and virtual package offsets are not compressed file offsets.',
            'No original SHA revalidation performed by this index generator; independent '
            'streamed original-file validation is a separate gate.',
        ],
    }
    products = {
        'native-functions.json': nodes, 'native-call-edges.json': edges,
        'map-summary.json': summary,
    }
    for relative, sha in inputs.items():
        if digest(root / relative) != sha:
            raise ValueError('Pinned input changed before output: ' + relative)
    for filename, value in products.items():
        (output / filename).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n',
                                      encoding='utf-8')
    write_csv(output / 'pe-module-edges.csv', linkage, list(linkage[0]))
    write_csv(output / 'native-occurrences.csv', occurrences,
              ['original_sha256', 'entry_va', 'rva', 'source', 'source_sha256',
               'line', 'pseudocode_sha256', 'evidence_id'])
    write_csv(output / 'report-sections.csv', report_index, list(report_index[0]))
    (output / 'outputs.json').write_text(json.dumps({p.name: digest(p) for p in sorted(output.iterdir())
        if p.is_file() and p.name != 'outputs.json'}, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k not in
        ['inputs', 'limits', 'retained_failure_records', 'jsonl_parse_errors',
         'non_dossier_logs_with_jsonl_suffix']}, indent=2))
    print(json.dumps({'retained_failures': len(failures), 'parse_errors': len(parse_errors)}))
    if parse_errors:
        raise ValueError('Malformed JSONL remains explicit; index is not fully accepted')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build(Path(__file__).resolve().parents[3], args.output.resolve())
