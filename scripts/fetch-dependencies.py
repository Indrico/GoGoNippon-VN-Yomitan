#!/usr/bin/env python3
"""Fetch pinned upstream archives and verify their SHA-256 before use."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / '.cache'
ARCHIVES = {
    'yomitan-chrome.zip': (
        'https://github.com/yomidevs/yomitan/releases/download/26.10.12.0/yomitan-chrome.zip',
        '270bf736862624a6f66d86ce6efb3e76307df4a6ad034073134897de8a414aa7'),
    'minhook.tar.gz': (
        'https://codeload.github.com/TsudaKageyu/minhook/tar.gz/refs/tags/v1.3.4',
        '1aebeae4ca898330c507860acc2fca2eb335fe446a3a2b8444c3bf8b2660a14e'),
    'yomitan-source.tar.gz': (
        'https://codeload.github.com/yomidevs/yomitan/tar.gz/refs/tags/26.10.12.0',
        '814021bb323e1d9ee5ac301b3306ea765e3e742ed07efb098b81dd85337bb411'),
}


def fetch(name):
    url, expected = ARCHIVES[name]
    CACHE.mkdir(exist_ok=True)
    target = CACHE / name
    if not target.exists():
        print('Downloading', name, flush=True)
        temporary = target.with_suffix(target.suffix + '.download')
        try:
            request = urllib.request.Request(url, headers={'User-Agent': 'GoGoNippon-Study-build'})
            with urllib.request.urlopen(request, timeout=120) as response, temporary.open('wb') as output:
                shutil.copyfileobj(response, output)
            with temporary.open('rb') as content:
                if hashlib.file_digest(content, 'sha256').hexdigest() != expected:
                    raise ValueError(f'Checksum mismatch for {name}')
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    with target.open('rb') as content:
        if hashlib.file_digest(content, 'sha256').hexdigest() != expected:
            raise ValueError(f'Checksum mismatch: remove {target} and retry')
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', action='store_true', help='also fetch the Yomitan upstream source for distribution')
    args = parser.parse_args()
    fetch('yomitan-chrome.zip')
    archive = fetch('minhook.tar.gz')
    destination = ROOT / 'vendor/minhook-1.3.4'
    if destination.exists():
        shutil.rmtree(destination)
    with tarfile.open(archive) as content:
        content.extractall(ROOT / 'vendor', filter='data')
    if args.source:
        fetch('yomitan-source.tar.gz')
    print('Upstream dependencies verified.')


if __name__ == '__main__':
    main()
