#!/usr/bin/env python3
"""Collect corresponding source and checksums for the current Windows release."""
import hashlib
import argparse
import json
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / 'dist'
VERSION = json.loads((ROOT / 'package.json').read_text())['version']
PREFIX = f'GoGoNippon-Study-{VERSION}'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--update-only', action='store_true', help='collect an update and source after package:win, without full installers')
args = parser.parse_args()
assets = [] if args.update_only else [DIST / f'{PREFIX}-Windows-x64-Setup.exe', DIST / f'{PREFIX}-Windows-x64-Portable.7z']
if not all(file.is_file() for file in assets):
    raise SystemExit('Run npm run package:dist first.')
base_version = {'0.3.1': '0.2.2', '0.3.2': '0.3.1'}.get(VERSION)
if args.update_only and not base_version:
    raise SystemExit('No compatible update base is defined for this version.')
packaged = subprocess.check_output(['node', '-e', "process.stdout.write(JSON.parse(require('@electron/asar').extractFile('dist/win-unpacked/resources/app.asar','package.json')).version)"], cwd=ROOT, text=True)
if packaged != VERSION:
    raise SystemExit('The unpacked app version does not match. Run npm run package:win first.')
# Reuse the pinned Electron, extension and reading dictionary from this base.
if base_version:
    update = DIST / f'{PREFIX}-Update-from-{base_version}.zip'
    with zipfile.ZipFile(update, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for filename in ['resources/app.asar', 'resources/native/study-helper.exe', 'resources/native/study-hook.dll', 'resources/profiles/bgi-1.528.ini']:
            archive.write(DIST / 'win-unpacked' / filename, filename)
        for filename in ['LICENSE', 'THIRD_PARTY.md']:
            archive.write(ROOT / filename, filename)
        archive.writestr('UPDATE.txt', f'''GoGoNippon Study {VERSION} - update from {base_version} only

Use this ZIP only with the complete official GoGoNippon Study {base_version}
Windows distribution (Electron 37.10.3, Yomitan 26.10.12.0).
For a fresh install, use the full Setup.exe or Portable.7z instead.

1. Close both the launcher and BGI.exe to release the loaded native DLL.
2. Extract this ZIP into the existing launcher folder, beside
   GoGoNippon-Study.exe. Merge resources and replace matching files.
3. Run the existing GoGoNippon-Study.exe. The footer should show {VERSION}.

Your game path, settings and imported dictionaries remain in
%APPDATA%\\gogonippon-yomitan. This update does not replace Electron or
prune the old installation's locales, so its disk footprint stays similar.

Corresponding source and full packages:
https://github.com/Indrico/GoGoNippon-VN-Yomitan/releases/tag/v{VERSION}
''')
    assets.append(update)
subprocess.run([sys.executable, str(ROOT / 'scripts/fetch-dependencies.py'), '--source'], check=True)
source = DIST / f'{PREFIX}-Source.zip'
base = Path(PREFIX + '-Source')
with zipfile.ZipFile(source, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for directory in ['src', 'scripts', 'assets', 'docs/screenshots', 'profiles', 'native', '.github', 'vendor/minhook-1.3.4']:
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
