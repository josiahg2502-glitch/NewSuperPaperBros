#!/usr/bin/env python3
"""Build and run the libretro core's runtime path on macOS ARM64 (windowless, Metal Dawn)."""
import argparse
import json
import os
import platform
from pathlib import Path
import shutil
import subprocess
import pipeline as shared

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('stage', choices=['configure', 'build', 'run', 'all'])
p.add_argument('--jobs', type=int, default=4)
p.add_argument('--frames', type=int, default=3600)
p.add_argument('--cycles', type=int, default=1)
p.add_argument('--timeout', type=int, default=120, help='bounded runtime test in seconds')
p.add_argument('--gfx-flags', default='', help='AURORA_GFX_FLAGS for the run')
p.add_argument('--output', type=Path, default=shared.ROOT / 'build-logs/host/first-run')
p.add_argument('--saves-from', type=Path, help='copy this earlier run\'s saves folder first')
p.add_argument('--dawn', type=Path, default=os.environ.get('DAWN_MACOS_ARCHIVE'),
               help='local dawn-darwin-arm64.tar.gz; Aurora downloads its pinned build otherwise')
args = p.parse_args()
if platform.system() != 'Darwin' or platform.machine() != 'arm64':
    raise SystemExit('This diagnostic target requires macOS ARM64')
shared.STATE = shared.ROOT / 'build-logs/host'
build = shared.WC / 'native-build/macos-diagnostic'
deps = shared.WC / 'native-build/android/_deps'
expected = '0a8ea8eb0159fc0ba1083c52155d9376fb173cffe690b400464a6ad8881bb461'
if args.dawn and shared.digest(args.dawn) != expected:
    raise SystemExit('macOS Dawn archive digest mismatch')
configure = ['cmake', '-S', shared.WC / 'runtime', '-B', build, '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_C_COMPILER=/usr/bin/clang',
             '-DCMAKE_CXX_COMPILER=/usr/bin/clang++', '-DCMAKE_OSX_ARCHITECTURES=arm64',
             '-DCMAKE_CXX_FLAGS=-gline-tables-only', '-DMKW_FRONTEND_DIAGNOSTIC=ON',
             '-DMKW_BUILD_LIBRETRO=OFF', '-DAURORA_DAWN_PROVIDER=package',
             '-DAURORA_DAWN_LINKAGE=static', '-DAURORA_ENABLE_CARD=OFF',
             '-DAURORA_SDL3_PROVIDER=vendor', f'-DMKW_TRANSLATED_COMPILE_JOBS={args.jobs}']
if args.dawn:
    configure += [f'-DAURORA_DAWN_PACKAGE_URL={args.dawn.resolve()}', f'-DAURORA_DAWN_PACKAGE_URL_HASH=SHA256={expected}']
# Reuse the Android build's downloaded dependency sources when they are there.
for name in ['abseil-cpp', 'fmt', 'freetype', 'imgui', 'mkw_mbedtls_upstream', 'png',
             'sdl', 'sqlite3', 'tracy', 'xxhash', 'zstd']:
    source = deps / (name + '-src')
    if source.is_dir():
        configure.append(f'-DFETCHCONTENT_SOURCE_DIR_{name.upper()}={source}')
if args.stage in ('configure', 'all'):
    shared.stage('configure', {'command': list(map(str, configure)),
        'cmake': shared.tree(shared.WC / 'runtime/CMakeLists.txt', shared.WC / 'runtime/cmake',
                            shared.WC / 'aurora-main/cmake'), 'dawn': expected},
        lambda log: shared.run(configure, log), [build / 'build.ninja', build / 'CMakeCache.txt'])
if args.stage in ('build', 'all'):
    shared.stage('build', {'source': shared.tree(shared.WC / 'runtime', shared.WC / 'aurora-main',
                                               shared.WC / 'generated'),
        'configure': shared.digest(shared.STATE / 'configure.json')},
        lambda log: shared.run(['cmake', '--build', build, '--target', 'WiiCompiled',
                               '--parallel', args.jobs], log), [build / 'WiiCompiled'])
if args.stage in ('run', 'all'):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    system = output / 'system'
    system.mkdir()
    if args.saves_from: shutil.copytree(args.saves_from / 'saves', output / 'saves')
    for name in ('dsp_coef.bin', 'cacert.pem', 'wii_bootstrap'):
        source = build / name
        if source.is_dir(): shutil.copytree(source, system / name)
        else: shutil.copy2(source, system / name)
    command = [build / 'WiiCompiled', '--frontend-diagnostic', shared.ROOT / 'Disc/DATA',
               system, output, str(args.frames)]
    with (output / 'runtime.log').open('w') as log:
        env = os.environ.copy()
        env['AURORA_GFX_FLAGS'] = args.gfx_flags
        env['SPM_FRONTEND_CYCLES'] = str(args.cycles)
        try:
            result = subprocess.run(list(map(str, command)), cwd=output, env=env,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
        except subprocess.TimeoutExpired:
            result = subprocess.CompletedProcess(command, 124)
    (output / 'result.json').write_text(json.dumps({'exit_code': result.returncode,
        'binary_sha256': shared.digest(build / 'WiiCompiled'), 'requested_frames': args.frames, 'cycles': args.cycles,
        'backend': 'Metal', 'architecture': 'arm64', 'graphics_debug_flags': args.gfx_flags,
        }, indent=2) + '\n')
    print(f'{"PASS" if result.returncode == 0 else "FAIL"} host-run: {output / "result.json"}')
    raise SystemExit(result.returncode)
