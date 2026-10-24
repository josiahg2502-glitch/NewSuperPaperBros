#!/usr/bin/env python3
"""Verify a real linked Android core: ABI, exports, dependencies and 16KB segments."""
import argparse
import hashlib
import pathlib
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("core", type=pathlib.Path)
parser.add_argument("--ndk", type=pathlib.Path, required=True)
parser.add_argument("--allow-dependency", action="append", choices=["libz.so"], default=[],
                    help="Additional Android system library used by the full GX renderer")
args = parser.parse_args()
reader = next(args.ndk.glob("toolchains/llvm/prebuilt/*/bin/llvm-readelf"))
def read(*flags):
    return subprocess.check_output([str(reader), *flags, str(args.core)], text=True)
header = read("-h")
assert "AArch64" in header and "DYN (Shared object file)" in header, header
symbols = read("--dyn-syms", "--wide")
exports = {line.split()[-1] for line in symbols.splitlines() if ("GLOBAL" in line or "WEAK" in line) and "DEFAULT" in line and " UND " not in line}
expected = {"retro_" + name for name in "api_version init deinit set_environment set_video_refresh set_audio_sample set_audio_sample_batch set_input_poll set_input_state get_system_info get_system_av_info set_controller_port_device reset run serialize_size serialize unserialize cheat_reset cheat_set load_game load_game_special unload_game get_region get_memory_data get_memory_size".split()}
assert exports == expected, ("wrong libretro exports", expected - exports, exports - expected)
deps = set(re.findall(r"\(NEEDED\).*?\[(.*?)\]", read("-d")))
allowed = {"libvulkan.so", "libandroid.so", "liblog.so", "libdl.so", "libm.so", "libc.so"}
allowed.update(args.allow_dependency)
assert deps <= allowed and {"libvulkan.so", "libandroid.so", "liblog.so"} <= deps, deps
loads = [line.split() for line in read("-l", "--wide").splitlines() if line.strip().startswith("LOAD ")]
assert loads and all(int(line[-1], 16) >= 16384 for line in loads), loads
digest = hashlib.sha256(args.core.read_bytes()).hexdigest()
print(f"PASS: Android AArch64 ELF, {len(exports)} libretro exports, static C++ runtime, >=16KB LOAD alignment")
print("Dependencies:", ", ".join(sorted(deps)))
print("SHA256:", digest)
