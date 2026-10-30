"""Run an SPM map sweep in an isolated portable folder; restart after failures.

Per map it records the renderer audit counters ([spmaudit]: draws, dropped draws, textures with no
GPU handle, loads to invalid texture slots) and captures the game window (PrintWindow, no desktop
input) to measure how much of the frame is black. Maps with any of those are listed as render
issues; shots/ holds a 480px screenshot of every map for review.
"""
import argparse
import collections
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

AUDIT = re.compile(r'\[spmaudit\] (\S+) draws=(\d+) dropped=(\d+) nohandle=(\d+) badtexmap=(\d+)(?: clear=([0-9A-Fa-f]{6}))?(?: fade=(\d+))?(?: xfskip=(\d+) nesteddl=(\d+))?(?: texnull=(\d+) palnull=(\d+))?')


def capture(pid, path):
    """Window-only capture of the game (no desktop input); returns (black, undrawn) pixel fractions or None.
    In sweep mode the renderer clears to magenta, so magenta pixels are pixels nothing drew."""
    if os.name != 'nt':
        return None
    try:
        import ctypes
        import ctypes.wintypes as w
        user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
        try:
            # else GetClientRect is DPI-scaled and the shot is a top-left crop of the window. Per-monitor v2 like the
            # game: system-aware alone still cropped to a third on a 4K screen at 300%.
            if not user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
                user32.SetProcessDPIAware()
        except Exception:
            pass
        user32.GetDC.restype = w.HDC
        user32.GetDC.argtypes = [w.HWND]
        user32.ReleaseDC.argtypes = [w.HWND, w.HDC]
        user32.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
        user32.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
        user32.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        user32.IsWindowVisible.argtypes = [w.HWND]
        gdi32.CreateCompatibleDC.restype = w.HDC
        gdi32.CreateCompatibleDC.argtypes = [w.HDC]
        gdi32.CreateCompatibleBitmap.restype = w.HBITMAP
        gdi32.CreateCompatibleBitmap.argtypes = [w.HDC, ctypes.c_int, ctypes.c_int]
        gdi32.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
        gdi32.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, ctypes.c_void_p, ctypes.c_void_p, w.UINT]
        gdi32.DeleteObject.argtypes = [w.HGDIOBJ]
        gdi32.DeleteDC.argtypes = [w.HDC]
        found = []
        proto = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)

        seen = []  # (hwnd, visible) of every window the pid owns, for the failure message

        def cb(h, _):
            owner = w.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(owner))
            if owner.value == pid:
                seen.append((h, bool(user32.IsWindowVisible(h))))
                if user32.IsWindowVisible(h):
                    found.append(h)
            return True
        # the game window is the largest visible one the process owns (tool/overlay windows are small or empty);
        # it is occasionally absent or 0-sized for a moment, so retry briefly
        def client(h):
            r = w.RECT()
            user32.GetClientRect(h, ctypes.byref(r))
            return r.right, r.bottom
        width = height = 0
        for attempt in range(8):
            found.clear()
            seen.clear()
            enum_cb = proto(cb)
            ok = user32.EnumWindows(enum_cb, 0)
            if attempt == 7:
                print(f'capture: EnumWindows={ok} lasterr={ctypes.get_last_error()} pid windows={seen}', file=sys.stderr, flush=True)
            if not found:  # this process sometimes cannot enumerate its child's window at all; PowerShell can
                try:
                    h = int(subprocess.check_output(['powershell', '-NoProfile', '-Command', f'(Get-Process -Id {pid}).MainWindowHandle'],
                                                    creationflags=0x08000000, timeout=20).decode().strip() or '0')
                except Exception:
                    h = 0
                if h:
                    found.append(h)
            if found:
                hwnd = max(found, key=lambda h: client(h)[0] * client(h)[1])
                width, height = client(hwnd)
                if width > 0 and height > 0:
                    break
            time.sleep(0.5)
        if width <= 0 or height <= 0:
            print(f'capture: no game window for pid {pid} (found {len(found)})', file=sys.stderr, flush=True)
            return None
        hdc = user32.GetDC(hwnd)
        mdc = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, width, height)
        gdi32.SelectObject(mdc, bmp)
        user32.PrintWindow(hwnd, mdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT

        class BIH(ctypes.Structure):
            _fields_ = [('biSize', w.DWORD), ('biWidth', w.LONG), ('biHeight', w.LONG), ('biPlanes', w.WORD),
                        ('biBitCount', w.WORD), ('biCompression', w.DWORD), ('biSizeImage', w.DWORD),
                        ('biXPelsPerMeter', w.LONG), ('biYPelsPerMeter', w.LONG), ('biClrUsed', w.DWORD),
                        ('biClrImportant', w.DWORD)]
        bih = BIH(ctypes.sizeof(BIH), width, -height, 1, 32, 0, 0, 0, 0, 0, 0)
        buf = ctypes.create_string_buffer(width * height * 4)
        gdi32.GetDIBits(mdc, bmp, 0, height, buf, ctypes.byref(bih), 0)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mdc)
        user32.ReleaseDC(hwnd, hdc)
        raw = buf.raw
        step = 1 if os.environ.get('SPM_SWEEP_FULLSHOT') else max(1, width // 480)  # full-size shots for edge inspection
        ow, oh = width // step, height // step
        black = magenta = total = 0
        rows = []
        for y in range(oh):
            row = bytearray()
            base = y * step * width * 4
            for x in range(ow):
                i = base + x * step * 4
                b, g, r = raw[i], raw[i + 1], raw[i + 2]
                row += bytes((b, g, r))
                total += 1
                black += r < 14 and g < 14 and b < 14
                magenta += r > 200 and g < 60 and b > 200
            row += b'\0' * ((4 - len(row) % 4) % 4)
            rows.append(bytes(row))
        data = b''.join(reversed(rows))  # BMP rows are bottom-up
        header = b'BM' + (54 + len(data)).to_bytes(4, 'little') + b'\0\0\0\0' + (54).to_bytes(4, 'little')
        info = (40).to_bytes(4, 'little') + ow.to_bytes(4, 'little') + oh.to_bytes(4, 'little') + (1).to_bytes(2, 'little') + \
            (24).to_bytes(2, 'little') + bytes(4) + len(data).to_bytes(4, 'little') + bytes(16)
        Path(path).write_bytes(header + info + data)
        return round(black / max(total, 1), 3), round(magenta / max(total, 1), 3)
    except Exception:
        return None


MARKER = re.compile(r'\[spmsweep\] (\S+)(?: (\S+))?(.*)')
PROBLEM = re.compile(r'OS::Panic|OS::Fatal|assertion failed|invalid tex map|unimplemented GX|'
                     r'not translated|out of bounds|rejected reason|\[error\]|\[fatal\]|\[spmsweep\] fatal', re.I)


def fingerprint(path):
    return {p.relative_to(path).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in path.rglob('*') if p.is_file()} if path.exists() else {}


def write_report(out, maps, results, original_unchanged, reason=''):
    counts = collections.Counter(r['status'] for r in results)
    data = dict(requested=len(maps), attempted=len(results), remaining=len(maps)-len(results),
                counts=dict(counts), original_saves_unchanged=original_unchanged, stopped_reason=reason,
                results=results)
    (out / 'results.json').write_text(json.dumps(data, indent=2), encoding='utf-8')
    lines = ['# SPM map-load sweep', '', f'{len(results)}/{len(maps)} maps processed.', '',
             *[f'- {k}: {v}' for k, v in sorted(counts.items())], '',
             f'Original save files unchanged: {original_unchanged}.', '',
             'The runner uses a private copy of the saves and internal Wii Remote input. Native textures are used.',
             'Sampled means the requested map became active and presented at least ten frames over the dwell interval.',
             'This does not verify pixels, story flags, entrances, every camera, combat or all special effects. Maps may require story state or specific entrances.',
             'Warnings are log observations, not automatically attributed to a renderer bug. Some runtime diagnostics stop logging after a fixed count.', '',
             reason, '', f"Render issues (dropped draws, missing textures, bad texture slots, >45% black; check clear= and the screenshot): "
             f"{sum(1 for r in results if r.get('render'))} maps. Screenshots: shots/<map>.bmp.", '',
             '| Map | Result | Detail |', '|---|---|---|']
    for r in results:
        detail = r.get('detail', '')
        if r.get('problems'):
            detail += '; ' + ' / '.join(r['problems'][:2])
        if r.get('render'):
            detail += '; RENDER: ' + ', '.join(r['render'])
        lines.append(f'| {r["map"]} | {r["status"]} | {detail.replace("|", "/")} |')
    (out / 'REPORT.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')


def prepare(root, out, exe, disc):
    play = out / 'Play'
    play.mkdir(parents=True, exist_ok=False)
    original = root / 'Play'
    shutil.copy2(exe, play / 'SuperPaperMario.exe')
    for file in original.iterdir():
        if file.is_file() and (file.suffix.lower() == '.dll' or file.name in ('dsp_coef.bin', 'initial_pipeline_cache.db')):
            shutil.copy2(file, play / file.name)
    (play / 'portable.txt').write_text('Isolated SPM map sweep\n')
    bootstrap = original / 'wii_bootstrap'
    if not bootstrap.is_dir():
        raise FileNotFoundError(f'Missing runtime bootstrap payload: {bootstrap}')
    shutil.copytree(bootstrap, play / 'wii_bootstrap')
    if (original / 'game_files').is_dir():  # built-in overlay (packaged HQ music)
        shutil.copytree(original / 'game_files', play / 'game_files')
    data = play / 'UserData'
    data.mkdir()
    for name in ('Saves', 'NAND', 'mods', 'cheats', 'button_icons') + (('texture_replacements',) if os.environ.get('SPM_SWEEP_HDPACK') else ()):
        if (original / 'UserData' / name).exists():
            shutil.copytree(original / 'UserData' / name, data / name)
    config = (original / 'UserData/Config.toml').read_text(encoding='utf-8')
    # aspect_mode 2 (fill) and no interpolation: letterbox bars would count as black, inserted frames as draws.
    # SPM_SWEEP_WINDOW=1920x1080 and SPM_SWEEP_RES=2.25: bigger window / render scale for promo screenshots.
    win_w, win_h = (os.environ.get('SPM_SWEEP_WINDOW') or '1280x720').lower().split('x')
    for key, value in dict(window_width=win_w, window_height=win_h, texture_replacements=os.environ.get('SPM_SWEEP_TEXREPL', 'false'), texture_dumps=os.environ.get('SPM_SWEEP_TEXDUMP', 'false'),
                           display_mode='"windowed"', resolution_multiplier=os.environ.get('SPM_SWEEP_RES', '1.0'),
                           aspect_mode=os.environ.get('SPM_SWEEP_ASPECT', '2'),
                           frame_interpolation_fps=os.environ.get('SPM_SWEEP_INTERP', '0'),
                           vsync=os.environ.get('SPM_SWEEP_VSYNC', 'false'),  # true to test V-Sync pacing
                           # off-screen (PrintWindow still captures it) so sweeps don't cover the user's desktop; SPM_SWEEP_ONSCREEN=1 shows it
                           # SPM_SWEEP_WINDOW_POS=x,y: another hidden spot, e.g. 0,3000 below a 240 Hz primary screen (its vblank timing)
                           window_x=(os.environ.get('SPM_SWEEP_WINDOW_POS') or ('100,' if os.environ.get('SPM_SWEEP_ONSCREEN') else '-4000,')).split(',')[0],
                           window_y=(os.environ.get('SPM_SWEEP_WINDOW_POS') or '0,100').split(',')[1],
                           graphics_api=f'"{os.environ.get("SPM_SWEEP_GRAPHICS", "auto")}"').items():  # auto | d3d12 | vulkan
        config, n = re.subn(rf'(?m)^{key}\s*=.*$', f'{key} = {value}', config)
        if not n:  # key absent from the user's file: add it under [video]
            config = config.replace('[video]\n', f'[video]\n{key} = {value}\n', 1)
    if os.environ.get('SPM_SWEEP_WIDESCREEN'):  # diagnostic: true/false
        config = re.sub(r'(?m)^widescreen\s*=.*$', f"widescreen = {os.environ['SPM_SWEEP_WIDESCREEN']}", config)
    config = re.sub(r"(?m)^dvd_root\s*=.*$", lambda _: f"dvd_root = '{disc}'", config)  # --disc (the PAL dump in Disc_PAL)
    if os.environ.get('SPM_SWEEP_LANGUAGE'):  # PAL engine: en fr de es it ([game] language, SCGetLanguage)
        config = re.sub(r'(?ms)\n\[game\].*?(?=\n\[|\Z)', '', config) + f"\n[game]\nlanguage = \"{os.environ['SPM_SWEEP_LANGUAGE']}\"\n"
    if not re.search(r'(?m)^\[audio\]', config):  # silent: the user works while sweeps run
        config += '\n[audio]\nmuted = true\n'
    else:
        config = re.sub(r'(?m)^\[audio\]\n', '[audio]\nmuted = true\n', re.sub(r'(?m)^muted\s*=.*\n', '', config), count=1)
    (data / 'Config.toml').write_text(config, encoding='utf-8')
    return play


def run(args):
    root = args.root.resolve()
    disc = (args.disc or root / 'Disc/DATA').resolve()
    names = sorted(p.stem for p in (disc / 'files/map').glob('*.bin'))
    maps = args.maps.split(',') if args.maps else names
    if not maps or len(set(maps)) != len(maps) or any(m not in names for m in maps):
        raise ValueError('Maps must be unique names from Disc/DATA/files/map')
    if args.limit:
        maps = maps[:args.limit]
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    out = (args.out or root / 'autotest' / f'map-sweep-{stamp}').resolve()
    out.mkdir(parents=True, exist_ok=False)
    saves = root / 'Play/UserData/Saves'
    before = fingerprint(saves)
    play = prepare(root, out, args.exe or root / 'Play/SuperPaperMario.exe', disc)
    results, index, boot_failures, launches = [], 0, 0, 0
    reason = ''
    print(f'Sweep: {len(maps)} maps; output: {out}', flush=True)
    process = None
    try:
        while index < len(maps):
            launches += 1
            remaining = out / 'remaining.txt'
            remaining.write_text('\n'.join(maps[index:]) + '\n', encoding='ascii')
            env = os.environ.copy()
            env['SPM_MAP_SWEEP_FILE'] = str(remaining)
            env.setdefault('AURORA_INTERP_NOCAP', '1')  # SPM_SWEEP_INTERP interpolates on a 60 Hz test monitor too
            env['SPM_MAP_SWEEP_SECONDS'] = str(args.seconds)
            env['NSMBW_INPUT_SCRIPT'] = os.environ.get('SPM_SWEEP_INPUT') or ','.join(f'{ms}:TWO:100' for ms in range(1000, 85000, 2000)) + ',5000:PLUS:100'
            env.pop('NSMBW_SESSION_SCRIPT', None)
            env.pop('NSMBW_WARP', None)
            log = out / f'launch-{launches:03}.log'
            err = out / f'launch-{launches:03}.err.log'
            with log.open('wb') as stdout, err.open('wb') as stderr:
                process = subprocess.Popen([str(play / 'SuperPaperMario.exe')], cwd=play, env=env,
                                           stdout=stdout, stderr=stderr, creationflags=0x08000000 if os.name == 'nt' else 0)
                progress = time.monotonic()
                current, problems, console, offset, tail, last_event, detail = None, [], None, 0, '', '', ''
                audit_counts = {}

                def consume(text):
                    nonlocal current, problems, index, progress, last_event, detail
                    for line in text.splitlines():
                        audit = AUDIT.search(line)
                        if audit:
                            audit_counts[audit.group(1)] = dict(zip(('draws', 'dropped', 'nohandle', 'badtexmap'),
                                                                    map(int, audit.groups()[1:5])))
                            audit_counts[audit.group(1)]['clear'] = audit.group(6)  # game's own clear color (dark map if 000000)
                            audit_counts[audit.group(1)]['xfskip'] = int(audit.group(8) or 0); audit_counts[audit.group(1)]['nesteddl'] = int(audit.group(9) or 0)
                            audit_counts[audit.group(1)]['texnull'] = int(audit.group(10) or 0); audit_counts[audit.group(1)]['palnull'] = int(audit.group(11) or 0)
                            audit_counts[audit.group(1)]['fadeDone'] = audit.group(7)  # '0' = fade curtain still up at sample time
                            continue
                        if PROBLEM.search(line) and len(problems) < 20:
                            problems.append(line.strip()[:450])
                        marker = MARKER.search(line)
                        if not marker:
                            continue
                        event, name, extra = marker.groups()
                        last_event = event
                        if event == 'attempt':
                            if name != maps[index]:
                                raise RuntimeError(f'Unexpected map order: {name}, expected {maps[index]}')
                            current, problems, detail = name, [], ''
                            progress = time.monotonic()
                        elif event in ('sampled', 'redirected') and current == name:
                            status = 'sampled-with-warnings' if event == 'sampled' and problems else event
                            result = dict(map=name, status=status, detail=extra.strip(), problems=problems,
                                          launch=launches, log=str(console))
                            if event == 'sampled':
                                (out / 'shots').mkdir(exist_ok=True)
                                black, undrawn = capture(process.pid, out / 'shots' / f'{name}.bmp') or (None, None)
                                a = audit_counts.get(name, {})
                                result.update(audit=a, black=black, undrawn=undrawn)
                                render = [f'{k}={a[k]}' for k in ('dropped', 'nohandle', 'badtexmap', 'xfskip', 'nesteddl', 'texnull', 'palnull') if a.get(k)]
                                # black is a hint only: check clear= (000000 = game clears to black) and the shot
                                if black is not None and black > 0.45:
                                    render.append(f'black={black:.0%}' + (' (fade still active)' if a.get('fadeDone') == '0' else ''))
                                result['render'] = render
                            results.append(result)
                            index += 1
                            current, problems = None, []
                            progress = time.monotonic()
                            print(f'{index}/{len(maps)} {name}: {status} {extra.strip()}', flush=True)
                            write_report(out, maps, results, fingerprint(saves) == before)
                        elif event in ('timeout', 'fatal', 'boot-timeout', 'allocation-failed'):
                            detail = line.strip()[:800]

                launched = time.monotonic()  # SPM_SWEEP_BOOTSHOTS=1,2,3: shots/boot-<s>.bmp at those seconds after launch
                boot_shots = [float(s) for s in os.environ.get('SPM_SWEEP_BOOTSHOTS', '').split(',') if s]
                while True:
                    while boot_shots and time.monotonic() - launched >= boot_shots[0]:
                        s = boot_shots.pop(0)
                        (out / 'shots').mkdir(exist_ok=True)
                        capture(process.pid, out / 'shots' / f'boot-{s:g}.bmp')
                    if console is None:
                        candidates = list((play / 'UserData/Logs').glob(f'*_pid{process.pid}/console.log'))
                        if candidates:
                            console = candidates[-1]
                    if console is not None:
                        with console.open('rb') as fh:
                            fh.seek(offset)
                            chunk = fh.read()
                            offset += len(chunk)
                        tail += chunk.decode('utf-8', errors='replace')
                        split = tail.rfind('\n')
                        if split >= 0:
                            consume(tail[:split+1])
                            tail = tail[split+1:]
                    exited = process.poll() is not None
                    timed_out = time.monotonic()-progress > (max(45, args.seconds + 40) if current else 100)
                    finished = current is None and index >= len(maps)  # every map sampled: close the game at once
                    if exited or timed_out or finished:
                        if not exited:
                            process.kill()
                            process.wait(timeout=10)
                        if tail:
                            consume(tail)
                        if current:
                            status = 'load-timeout' if timed_out or last_event == 'timeout' else 'failed'
                            results.append(dict(map=current, status=status,
                                                detail=detail or f'process exit {process.returncode}',
                                                problems=problems, launch=launches, log=str(console)))
                            index += 1
                            print(f'{index}/{len(maps)} {current}: {status} exit={process.returncode}', flush=True)
                            boot_failures = 0
                        elif index < len(maps):
                            boot_failures += 1
                            if boot_failures >= 2:
                                reason = 'Stopped after two launches without reaching a requested map.'
                        write_report(out, maps, results, fingerprint(saves) == before, reason)
                        break
                    time.sleep(0.25)
            if reason:
                break
    finally:
        if process and process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        write_report(out, maps, results, fingerprint(saves) == before, reason)
    print(out / 'REPORT.md', flush=True)
    return 0 if index == len(maps) else 2


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--out', type=Path)
    ap.add_argument('--exe', type=Path)
    ap.add_argument('--disc', type=Path, help='Extracted disc (DATA folder); default Disc/DATA. PAL: --disc Disc_PAL/DATA --exe Play/SuperPaperMarioPAL.exe')
    ap.add_argument('--maps', help='Comma-separated map names; default is every map file')
    ap.add_argument('--limit', type=int)
    ap.add_argument('--seconds', type=int, default=3, choices=range(1, 61))
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        assert MARKER.search('[runtime] [spmsweep] sampled mac_01 frames=180').groups() == ('sampled', 'mac_01', ' frames=180')
        assert PROBLEM.search('[gx] GXLoadTexObj: invalid tex map id 99')
        assert not PROBLEM.search('[vi] first visible guest frame presented')
        assert max(45, 60 + 40) > 60 + 30  # dwell and guest transition timeout fit the watchdog
        print('report parser self-checks passed')
    else:
        raise SystemExit(run(args))
