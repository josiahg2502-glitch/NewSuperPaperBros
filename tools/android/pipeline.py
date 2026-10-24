#!/usr/bin/env python3
"""Content-addressed Android build stages; logs and proprietary outputs stay ignored."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
WC = ROOT / 'wiicompiled'
STATE = ROOT / 'build-logs/android'
ORIGINAL = '0d7487bc387ee1e905be335acc1e25300eb28bb8949667f753f038dbd9cf53b3'
PATCHED = 'cd3b5662f61369d5ff946098033be69de8084a20b0dfd3c3d4f4819eed6d21ac'
DAWN = '0e63e8cbf53551f703f582d1306f4257c0380353f66b53369d96952ce6d9f934'


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def tree(*paths):
    out = {}
    for path in paths:
        path = Path(path)
        candidates = sorted(path.rglob('*')) if path.is_dir() else [path]
        for p in candidates:
            if p.is_file() and not any(x in ('bin', 'obj', '__pycache__') for x in (p.relative_to(path).parts if path.is_dir() else ())):
                out[str(p)] = digest(p)
    return out


def run(argv, log, cwd=ROOT):
    subprocess.run([str(x) for x in argv], cwd=cwd, stdout=log,
                   stderr=subprocess.STDOUT, check=True)


def stage(name, inputs, action, outputs):
    """Rehash inputs AND successful outputs; a missing/corrupt output invalidates the stage."""
    STATE.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    record = STATE / (name + '.json')
    if record.exists():
        old = json.loads(record.read_text())
        if old['key'] == key and all(Path(p).is_file() and digest(p) == h
                                    for p, h in old['outputs'].items()):
            print(f'PASS {name} cached: {record}', flush=True)
            return
    logfile = STATE / (name + '.log')
    try:
        with logfile.open('w') as log:
            action(log)
        artifacts = outputs() if callable(outputs) else outputs
        if not artifacts:
            raise RuntimeError('stage produced no artifacts')
        hashes = {str(p): digest(p) for p in artifacts}
        record.write_text(json.dumps({'key': key, 'inputs': inputs, 'outputs': hashes}, indent=2) + '\n')
        print(f'PASS {name}: {record}', flush=True)
    except Exception as e:
        lines = logfile.read_text(errors='replace').splitlines()
        useful = next((x for x in lines if any(s in x.lower() for s in ('error:', 'error ', 'fatal', 'exception'))), str(e))
        print(f'FAIL {name}: {useful}\nlog: {logfile}', file=sys.stderr)
        print('\n'.join(lines[-8:]), file=sys.stderr)
        raise SystemExit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['prepare-inputs', 'translator', 'translate', 'configure', 'build', 'verify', 'all'])
    parser.add_argument('--disc', type=Path, default=ROOT / 'resources/Super Paper Mario [R8PE01].wbfs')
    parser.add_argument('--dotnet', type=Path, default=ROOT / 'toolchain/dotnet/dotnet')
    parser.add_argument('--ndk', type=Path, default=os.environ.get('ANDROID_NDK_ROOT'), required='ANDROID_NDK_ROOT' not in os.environ)
    parser.add_argument('--dawn', type=Path, default=os.environ.get('DAWN_ANDROID_ARCHIVE'),
                        help='local dawn-android-aarch64.tar.gz; Aurora downloads its pinned build otherwise')
    parser.add_argument('--core', action='store_true', help='Build/verify the libretro target instead of the standalone runtime')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--arch', default='armv8-a', help='ISA floor: armv8-a, or armv8.2-a for devices that have it')
    args = parser.parse_args()
    os.environ.update(DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_CLI_HOME=str(ROOT / 'toolchain/dotnet-home'))
    scripts = tree(Path(__file__), ROOT / 'tools/extract_wbfs.py', WC / 'projects/spm/tools/prelink_spm_rel.py')
    game = ROOT / 'game'
    disc = ROOT / 'Disc/DATA'
    project = game / 'android-stock.yml'
    translator = WC / 'translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
    # The libretro core, the standalone app and each ISA build in separate trees.
    suffix = '' if args.arch == 'armv8-a' else '-' + args.arch.replace('.', '')
    build = WC / (('native-build/android' if args.core else 'native-build/android-app') + suffix)
    configure_stage = ('configure' if args.core else 'configure-app') + suffix
    selected = lambda s: args.stage in (s, 'all')
    if selected('prepare-inputs'):
        def prepare(log):
            # Reuse a complete extraction only when its own content manifest matches this disc.
            extract_record = STATE / 'extract.json'
            source_hash = digest(args.disc)
            extracted = tree(disc) if disc.exists() else {}
            cached = json.loads(extract_record.read_text()) if extract_record.exists() else {}
            if cached.get('source') != source_hash or cached.get('files') != extracted:
                disc.parent.mkdir(exist_ok=True)
                # An existing disc tree is never adopted or overwritten.
                with tempfile.TemporaryDirectory(prefix='android-extract-', dir=disc.parent) as temp:
                    fresh = Path(temp) / 'DATA'
                    run([sys.executable, ROOT / 'tools/extract_wbfs.py', args.disc, fresh], log)
                    if (fresh / 'sys/boot.bin').read_bytes()[:8] != b'R8PE01\x00\x00':
                        raise RuntimeError('requires R8PE01 revision 0')
                    fresh_hashes = {str(Path(p).relative_to(fresh)): h for p, h in tree(fresh).items()}
                    old_hashes = {str(Path(p).relative_to(disc)): h for p, h in extracted.items()}
                    if disc.exists():
                        if fresh_hashes != old_hashes:
                            raise RuntimeError('existing Disc/DATA differs from this disc; preserve it and choose a clean workspace')
                    else:
                        shutil.move(str(fresh), str(disc))
                extract_record.write_text(json.dumps({'source': source_hash, 'files': tree(disc)}, indent=2))
            game.mkdir(exist_ok=True)
            stock = (disc / 'sys/main.dol').read_bytes()
            if hashlib.sha256(stock).hexdigest() != ORIGINAL:
                raise RuntimeError('original DOL hash mismatch')
            orig = game / 'main.dol.orig'
            if orig.exists() and orig.read_bytes() != stock:
                raise RuntimeError('refusing to overwrite preserved original DOL')
            orig.write_bytes(stock)
            patched = bytearray(stock)
            for offset, before, after in ((0x21e518, 0x40820038, 0x48000038), (0x21e85c, 0x41820014, 0x48000014)):
                if struct.unpack_from('>I', patched, offset)[0] != before:
                    raise RuntimeError('unexpected patch input instruction')
                struct.pack_into('>I', patched, offset, after)
            if hashlib.sha256(patched).hexdigest() != PATCHED:
                raise RuntimeError('patched DOL hash mismatch')
            (game / 'main.dol').write_bytes(patched)
            run([sys.executable, WC / 'projects/spm/tools/prelink_spm_rel.py', disc / 'files/rel/relF.bin', game / 'spm_linked.rel', game / 'spm_rel_layout.json'], log)
            # Stock audio must retain stock pitch/rate code. Keep the HQ manifest and patched DOL intact.
            source = (WC / 'projects/spm/recomp.yml').read_text()
            source = source.replace('workspace_root: ../..', 'workspace_root: ../wiicompiled')
            source = source.replace('../game/main.dol', '../game/main.dol.orig').replace(PATCHED, ORIGINAL)
            source = '\n'.join(line.split('  # HQ audio')[0] for line in source.splitlines()) + '\n'
            project.write_text(source)
            (STATE / 'audio.json').write_text(json.dumps({'mode': 'stock', 'dol_sha256': ORIGINAL,
                'assets': tree(disc / 'files/sound'), 'overlay': None}, indent=2) + '\n')
        stage('prepare-inputs', {**scripts, 'disc': digest(args.disc), 'manifest': digest(WC / 'projects/spm/recomp.yml')}, prepare,
              lambda: [*map(Path, tree(disc)), game / 'main.dol.orig', game / 'main.dol', game / 'spm_linked.rel', game / 'spm_rel_layout.json', project, STATE / 'audio.json'])
    if selected('translator'):
        stage('translator', {**tree(WC / 'translator', WC / 'Launcher/WiiCompiled.Setup.Common'),
                            'sdk': subprocess.check_output([str(args.dotnet), '--version'], text=True).strip()},
              lambda log: run([args.dotnet, 'build', WC / 'translator/src/Translator.Cli/Translator.Cli.csproj', '-c', 'Release', '--no-restore'], log),
              lambda: [p for p in translator.parent.rglob('*') if p.is_file()])
    if selected('translate'):
        def translate(log):
            cli = [args.dotnet, translator]
            run([*cli, 'translate-recursive', '0x80006124', '--project', project, '--outdir', 'generated/functions', '--output-metadata', 'generated/base_translation_output.json', '--production-source-bundle', 'generated/base_translation_sources.bin', '--no-function-files', '--prune-stale', '--threads', args.jobs], log, WC)
            run([*cli, 'emit-base-manifest', '--project', project, '--out', 'build/base', '--functions-dir', 'generated/functions', '--translation-output-metadata', 'generated/base_translation_output.json', '--region', 'E'], log, WC)
            run([*cli, 'generate-data-init', '--project', project], log, WC)
            run([*cli, 'emit-build-shards', '--project', project, '--base-metadata', 'generated/base_translation_output.json', '--base-functions-dir', 'generated/functions', '--native-source-dir', 'runtime/src', '--out', 'generated/build_shards', '--resumable-functions', 'projects/spm/resumable_functions.inc'], log, WC)
        stage('translate', {**tree(translator.parent, project, WC / 'projects/spm/MAP.txt', WC / 'projects/spm/resumable_functions.inc', game / 'main.dol.orig', game / 'spm_linked.rel'),
                            'native': tree(WC / 'runtime/src', WC / 'runtime/include')}, translate,
              lambda: list(map(Path, tree(WC / 'generated', WC / 'build/base'))))
    if selected('configure'):
        if args.dawn and digest(args.dawn) != DAWN:
            raise SystemExit('Dawn archive hash mismatch')
        configure = ['cmake', '-S', WC / 'runtime', '-B', build, '-G', 'Ninja',
            f'-DCMAKE_TOOLCHAIN_FILE={args.ndk}/build/cmake/android.toolchain.cmake',
            '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-30',
            f'-DSDL_AUDIO={"OFF" if args.core else "ON"}', f'-DSDL_OPENGL={"OFF" if args.core else "ON"}',
            f'-DSDL_OPENGLES={"OFF" if args.core else "ON"}', '-DANDROID_STL=c++_static', f'-DMKW_ANDROID_ARCH={args.arch}',
            '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POSITION_INDEPENDENT_CODE=ON',
            '-DBUILD_SHARED_LIBS=OFF', '-DAURORA_DAWN_PROVIDER=package', '-DAURORA_DAWN_LINKAGE=static',
            *([f'-DAURORA_DAWN_PACKAGE_URL={args.dawn}', f'-DAURORA_DAWN_PACKAGE_URL_HASH=SHA256={DAWN}'] if args.dawn else []),
            f'-DMKW_TRANSLATED_COMPILE_JOBS={args.jobs}', f'-DMKW_BUILD_LIBRETRO={"ON" if args.core else "OFF"}', '-DAURORA_ENABLE_CARD=OFF']
        stage(configure_stage, {'command': list(map(str, configure)), 'ndk': tree(args.ndk / 'source.properties', args.ndk / 'build/cmake/android.toolchain.cmake', next((args.ndk / 'toolchains/llvm/prebuilt').glob('*/bin/clang'))),
                            'cmake': tree(WC / 'runtime/CMakeLists.txt', WC / 'runtime/cmake', WC / 'aurora-main/cmake', WC / 'aurora-main/CMakeLists.txt', WC / 'aurora-main/extern/CMakeLists.txt'),
                            'shards': digest(WC / 'generated/build_shards/shards.cmake')},
              lambda log: run(configure, log), [build / 'build.ninja', build / 'CMakeCache.txt'])
    target = 'spm_libretro' if args.core else 'WiiCompiled'
    product = build / ('spm_libretro.so' if args.core else 'libmain.so')
    if selected('build'):
        stage(('core-build' if args.core else 'build') + suffix, {'source': tree(WC / 'runtime', WC / 'aurora-main', WC / 'generated'),
                        'target': target, 'configure': digest(STATE / (configure_stage + '.json')), 'ndk': tree(args.ndk / 'source.properties', args.ndk / 'build/cmake/android.toolchain.cmake', next((args.ndk / 'toolchains/llvm/prebuilt').glob('*/bin/clang')))},
              lambda log: run(['cmake', '--build', build, '--target', target, '--parallel', args.jobs], log), [product])
    if selected('verify'):
        binary = product
        readelf = next((args.ndk / 'toolchains/llvm/prebuilt').glob('*/bin/llvm-readelf'))
        def verify(log):
            if args.core:
                run([sys.executable, ROOT / 'tools/android/verify-binary.py', binary, '--ndk', args.ndk, '--allow-dependency', 'libz.so'], log)
            output = subprocess.check_output([str(readelf), '-h', str(binary)], text=True)
            log.write(output)
            if 'AArch64' not in output or 'DYN' not in output:
                raise RuntimeError('runtime is not an AArch64 shared ELF')
            (STATE / ('runtime-metrics' + suffix + '.json')).write_text(json.dumps({'runtime_sha256': digest(binary),
                'bytes': binary.stat().st_size, 'target': 'Android ARM64'}, indent=2) + '\n')
        stage(('core-verify' if args.core else 'verify') + suffix, tree(binary, readelf), verify, [STATE / ('runtime-metrics' + suffix + '.json')])


if __name__ == '__main__':
    main()
