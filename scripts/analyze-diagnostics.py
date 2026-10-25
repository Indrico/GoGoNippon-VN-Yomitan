#!/usr/bin/env python3
"""Read a diagnostic ZIP without executing or extracting game code."""
import argparse
from collections import Counter
import json
import math
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('archive')
parser.add_argument('--max-samples', type=int, default=8)
args = parser.parse_args()
with zipfile.ZipFile(args.archive) as archive:
    names = set(archive.namelist())
    for name in sorted(names):
        if name.endswith('.json') and name.startswith('code-'):
            if archive.getinfo(name).file_size > 1024 * 1024:
                continue
            info = json.loads(archive.read(name))
            print(f"\nSnapshot {name}; image base 0x{info['imageBase']:08x}")
            for section in info['sections']:
                if section['file'] not in names or archive.getinfo(section['file']).file_size > 16 * 1024 * 1024:
                    continue
                data = archive.read(section['file'])
                entropy = -sum((n / len(data)) * math.log2(n / len(data)) for n in Counter(data).values()) if data else 0
                signature = bytes.fromhex('0f be c0 83 c0 fe 83 f8 06')
                hits = []; start = 0
                while len(hits) < 32:
                    position = data.find(signature, start)
                    if position < 0:
                        break
                    hits.append(hex(section['rva'] + position)); start = position + len(signature)
                print(f"  {section['file']}: RVA 0x{section['rva']:x}, {len(data)} bytes, entropy {entropy:.3f}, BGI parser patterns {hits}")
    if 'events.jsonl' in names:
        counts = Counter()
        printed = Counter()
        with archive.open('events.jsonl') as events:
            for raw in events:
                if len(raw) > 1024 * 1024:
                    continue
                try:
                    event = json.loads(raw)
                except (ValueError, UnicodeDecodeError):
                    continue
                kind = event.get('type')
                counts[kind] += 1
                if kind in ('candidate', 'candidate-scan', 'probe-site', 'window-geometry', 'engine-window-geometry'):
                    print('CANDIDATE', json.dumps(event, ensure_ascii=False))
                elif kind in ('gdi-sample', 'engine-sample', 'text-object-reset', 'text-layout-begin', 'text-layout-end', 'glyph-blit', 'text-layout-clear', 'text-layout-release', 'surface-clear', 'dialogue') and printed[kind] < max(0, args.max_samples):
                    sample = {key: value for key, value in event.items() if key not in ('surfaceBytes', 'objectBytes')}
                    print('SAMPLE', json.dumps(sample, ensure_ascii=False)[:5000])
                    printed[kind] += 1
        print('EVENT COUNTS', json.dumps(counts, ensure_ascii=False))
    print('\nPosition/clear hooks must be confirmed against the visible game; no profile is enabled by this reader.')
