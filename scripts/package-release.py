#!/usr/bin/env python3
"""Collect corresponding source and checksums for the current Windows release."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / 'dist'
VERSION = json.loads((ROOT / 'package.json').read_text())['version']
PREFIX = f'GoGoNippon-Study-{VERSION}'
assets = [DIST / f'{PREFIX}-Windows-x64-Setup.exe', DIST / f'{PREFIX}-Windows-x64-Portable.7z']
if not all(file.is_file() for file in assets):
    raise SystemExit('Run npm run package:dist first.')
subprocess.run([sys.executable, str(ROOT / 'scripts/fetch-dependencies.py'), '--source'], check=True)
source = DIST / f'{PREFIX}-Source.zip'
base = Path(PREFIX + '-Source')
with zipfile.ZipFile(source, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for directory in ['src', 'scripts', 'assets', 'profiles', 'native', '.github', 'vendor/minhook-1.3.4']:
        for file in sorted((ROOT / directory).rglob('*')):
            relative = file.relative_to(ROOT)
            if file.is_file() and not any(part in ['bin', 'obj', '__pycache__'] for part in relative.parts):
                archive.write(file, base / relative)
    for filename in ['package.json', 'package-lock.json', 'README.md', 'ENGINE-NOTES.md', 'CHANGELOG.md', 'THIRD_PARTY.md', 'LICENSE', '.gitignore', '.gitattributes']:
        archive.write(ROOT / filename, base / filename)
    # Preferred upstream source plus exact changed files; avoid duplicating
    # fonts, WASM and other unchanged generated assets from the Chrome package.
    archive.write(ROOT / '.cache/yomitan-source.tar.gz', base / 'upstream/yomitan-26.10.12.0.tar.gz', compress_type=zipfile.ZIP_STORED)
    for filename in ['manifest.json', 'GOGONIPPON-SOURCE.json', 'js/app/popup.js', 'js/app/frontend.js', 'js/language/text-scanner.js', 'js/data/permissions-util.js', 'js/background/backend.js']:
        archive.write(ROOT / 'vendor/yomitan' / filename, base / 'modified-yomitan' / filename)
assets.append(source)
checksums = []
for file in assets:
    with file.open('rb') as content:
        checksums.append(f'{hashlib.file_digest(content, "sha256").hexdigest()}  {file.name}\n')
    print(file.name, f'{file.stat().st_size / (1024 * 1024):.2f} MiB')
(DIST / 'SHA256SUMS.txt').write_text(''.join(checksums))
