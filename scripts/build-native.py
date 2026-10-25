#!/usr/bin/env python3
"""Build the 32-bit Windows helper and DLL using an existing/local MinGW SDK."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
SDK = ROOT / '.cache/mingw/usr'
LOCAL_CXX = SDK / 'bin/i686-w64-mingw32-g++-posix'
CXX = os.environ.get('STUDY_CXX') or shutil.which('i686-w64-mingw32-g++') or str(LOCAL_CXX)
CC = os.environ.get('STUDY_CC') or shutil.which('i686-w64-mingw32-gcc') or str(SDK / 'bin/i686-w64-mingw32-gcc-posix')
MINHOOK = ROOT / 'vendor/minhook-1.3.4'
BIN = ROOT / 'native/bin'
OBJ = ROOT / 'native/obj'
BIN.mkdir(exist_ok=True)
OBJ.mkdir(exist_ok=True)
if not MINHOOK.exists():
    raise SystemExit('Extract MinHook v1.3.4 to vendor/minhook-1.3.4 first (see README).')

environment = os.environ.copy()
environment['PATH'] = str(SDK / 'bin') + os.pathsep + environment.get('PATH', '')
common = ['-O2', '-s', '-Wall', '-Wextra', '-DUNICODE', '-D_UNICODE', '-D_WIN32_WINNT=0x0601', '-fno-strict-aliasing']
include = ['-I' + str(MINHOOK / 'include'), '-I' + str(MINHOOK / 'src')]
objects = []
for source in ['buffer.c', 'hook.c', 'trampoline.c', 'hde/hde32.c']:
    output = OBJ / (source.replace('/', '-') + '.o')
    subprocess.run([CC, *common, *include, '-c', str(MINHOOK / 'src' / source), '-o', str(output)], check=True, env=environment)
    objects.append(str(output))
subprocess.run([CXX, *common, '-std=c++17', '-static', '-static-libgcc', '-static-libstdc++', '-municode', str(ROOT / 'native/helper.cpp'), '-o', str(BIN / 'study-helper.exe'), '-luser32', '-lgdi32', '-lshell32'], check=True, env=environment)
subprocess.run([CXX, *common, *include, '-std=c++17', '-shared', '-static', '-static-libgcc', '-static-libstdc++', '-Wl,--kill-at', str(ROOT / 'native/hook.cpp'), *objects, '-o', str(BIN / 'study-hook.dll'), '-luser32', '-lgdi32'], check=True, env=environment)
print('Built', BIN)
