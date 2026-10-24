#!/usr/bin/env python3
"""Regenerate the Preparing screen's committed text atlas (frontend/prepare_text.inc)."""
import shutil
import subprocess
import tempfile
from pathlib import Path
from pipeline import ROOT, WC

imgui = WC / 'native-build/android/_deps/imgui-src'
font = imgui / 'misc/fonts/Roboto-Medium.ttf'
output = WC / 'runtime/frontend/prepare_text.inc'
compiler = shutil.which('clang++') or shutil.which('c++')
if not compiler or not font.is_file():
    raise SystemExit(f'Needs clang++ and the cached ImGui sources ({font})')
with tempfile.TemporaryDirectory() as temp:
    tool = Path(temp) / 'make-prepare-text'
    subprocess.run([compiler, '-std=c++17', '-O2', '-w', '-I', imgui,
                    ROOT / 'tools/android/make-prepare-text.cpp', '-o', tool], check=True)
    subprocess.run([tool, font, output], check=True)
print('PASS', output.relative_to(ROOT))
