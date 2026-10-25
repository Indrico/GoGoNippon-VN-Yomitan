#!/usr/bin/env python3
"""Replay user-supplied evidence through the DLL's portable ownership store.

Game text remains in .cache and is never bundled in a release.
"""
from pathlib import Path
import argparse
import json
import struct
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('archive', type=Path)
args = parser.parse_args()
with zipfile.ZipFile(args.archive) as archive:
    assert archive.testzip() is None, 'Bad ZIP CRC'
    events = [json.loads(line) for line in archive.read('events.jsonl').splitlines()]

owners, active, commands = {}, {}, []
raster_surfaces = set()
latest, page = None, 0
for event in events:
    kind = event['type']
    if kind == 'text-object-reset':
        root = event['object'] + 0x7c
        surface = int.from_bytes(bytes.fromhex(event['surfacePointer']), 'little')
        owners[root] = surface
        active.pop(surface, None)
        raster_surfaces.discard(surface)
        commands.append(f'R {surface}')
    elif kind == 'text-layout-end':
        root = event['root']
        surface = owners[root]
        nodes = event['nodes']
        assert len(nodes) == event['capturedGlyphs']
        assert ''.join(node['text'] for node in nodes) == event['text']
        active[surface] = event
        raster_surfaces.add(surface)
        commands.append(f'L {root} {surface} {len(nodes)}')
        for node in nodes:
            units = len(node['text'].encode('utf-16-le')) // 2
            commands.append(f"{node['address']} {node['pixels']} {node['width']} {node['height']} {units} {node['text'].encode().hex()}")
        commands.append(event['text'].encode().hex())
        page += 1
        latest = {'type': 'dialogue', 'page': page, 'sequence': 10000 + page, 'width': 800, 'height': 600,
                  'text': event['text'], 'glyphs': []}
    elif kind in ('text-layout-clear', 'text-layout-release'):
        root = event['root']
        commands.append(f'F {root}')
        for surface, layout in active.items():
            if layout['root'] == root:
                raster_surfaces.discard(surface)
    elif kind == 'glyph-blit':
        surface = event['surface']
        pixels, _, width, height = struct.unpack_from('<IIII', bytes.fromhex(event['bitmapBytes']))
        origin_x, origin_y = struct.unpack_from('<ii', bytes.fromhex(event['surfaceBytes']), 0x24)
        layout = active.get(surface)
        expected = bool(layout and layout['root'] == event['root'] and surface in raster_surfaces)
        commands.append(f"B {surface} {pixels} {width} {height} {event['x']} {event['y']} {event['right'] - event['x'] + 1} {event['bottom'] - event['y'] + 1} {origin_x} {origin_y} {int(expected)} {event['node']}")
        if expected:
            assert 0 <= event['x'] <= event['right'] < 800 and 0 <= event['y'] <= event['bottom'] < 600
            if latest and latest['text'] == layout['text']:
                index = next(i for i, node in enumerate(layout['nodes']) if node['address'] == event['node'])
                start = len(''.join(node['text'] for node in layout['nodes'][:index]).encode('utf-16-le')) // 2
                latest['glyphs'].append({'start': start, 'end': start + len(event['text'].encode('utf-16-le')) // 2,
                                         'x': event['x'], 'y': event['y'], 'width': event['right'] - event['x'] + 1,
                                         'height': event['bottom'] - event['y'] + 1})

with tempfile.TemporaryDirectory(prefix='gogonippon-replay-') as temporary:
    binary = str(Path(temporary) / 'check-layout')
    subprocess.run(['g++', '-std=c++17', '-O2', '-Wall', '-Wextra', str(ROOT / 'scripts/check-layout.cpp'), '-o', binary], check=True)
    subprocess.run([binary], input='\n'.join(commands) + '\n', text=True, check=True)

assert latest and latest['glyphs'], 'No complete layout/blit evidence'
latest['glyphs'].sort(key=lambda glyph: glyph['start'])
for glyph, following in zip(latest['glyphs'], latest['glyphs'][1:]):
    if glyph['y'] == following['y'] and following['x'] > glyph['x']:
        glyph['width'] = min(glyph['width'], following['x'] - glyph['x'])
output = ROOT / '.cache/replay-dialogue.json'
output.parent.mkdir(exist_ok=True)
output.write_text(json.dumps(latest, ensure_ascii=False, indent=2) + '\n')
print('Overlay replay fixture:', output)
