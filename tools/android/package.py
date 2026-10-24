#!/usr/bin/env python3
"""Package the development Super Paper Mario Android libretro core."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import traceback


ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOT = ROOT / "wiicompiled"
DEFAULT_CORE = SOURCE_ROOT / "native-build/android/spm_libretro.so"
DEFAULT_OUTPUT = ROOT / "out/android"
CORE_NAME = "spm_libretro_android.so"  # RetroArch Android names; matches spm_libretro.info
SYSTEM_FILES = ("dsp_coef.bin", "cacert.pem", "wii_bootstrap")
VERIFIER = ROOT / "tools/android/verify-binary.py"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(*args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(SOURCE_ROOT), *args], text=True
    ).strip()


def verify(core: Path, ndk: Path, log) -> None:
    log.write(f"$ {sys.executable} {VERIFIER} {core} --ndk {ndk}\n")
    log.flush()
    result = subprocess.run(
        [sys.executable, str(VERIFIER), str(core), "--ndk", str(ndk), "--allow-dependency", "libz.so"],
        stdout=log,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if result.returncode:
        raise RuntimeError(f"binary verifier failed (exit {result.returncode})")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, default=DEFAULT_CORE)
    parser.add_argument("--ndk", type=Path, default=os.environ.get("ANDROID_NDK_ROOT"), required="ANDROID_NDK_ROOT" not in os.environ)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--build-record", default="core-build", help="pipeline stage record of --core (core-build-armv82-a for an --arch armv8.2-a build)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    log_path = ROOT / "build-logs/android/package.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        stage: Path | None = None
        backup: Path | None = None
        try:
            core = args.core.resolve(strict=True)
            ndk = args.ndk.resolve(strict=True)
            output = args.output.expanduser().absolute()
            if not core.is_file():
                raise ValueError(f"core is not a file: {core}")
            if not ndk.is_dir():
                raise ValueError(f"NDK path is not a directory: {ndk}")
            if not VERIFIER.is_file():
                raise ValueError(f"binary verifier not found: {VERIFIER}")

            known_output = output.resolve(strict=False) == DEFAULT_OUTPUT.resolve(strict=False)
            if output.exists() and not output.is_dir():
                raise ValueError(f"output exists and is not a directory: {output}")
            if output.is_dir() and any(output.iterdir()) and not known_output:
                raise ValueError(
                    f"refusing to replace non-empty unknown output directory: {output}"
                )

            output.parent.mkdir(parents=True, exist_ok=True)
            stage = Path(tempfile.mkdtemp(prefix=f".{output.name}.tmp-", dir=output.parent))
            log.write(f"Source core: {core}\nNDK: {ndk}\nOutput: {output}\n")
            verify(core, ndk, log)

            packaged_core = stage / CORE_NAME
            shutil.copy2(core, packaged_core)
            strip = next(ndk.glob("toolchains/llvm/prebuilt/*/bin/llvm-strip"), None)
            if strip is None:
                raise ValueError(f"NDK llvm-strip not found under {ndk}")
            log.write(f"$ {strip} --strip-unneeded {packaged_core}\n")
            log.flush()
            subprocess.run(
                [str(strip), "--strip-unneeded", str(packaged_core)],
                check=True,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
            )
            verify(packaged_core, ndk, log)

            shutil.copyfile(ROOT / "tools/android/spm_libretro.info", stage / "spm_libretro.info")
            shutil.copyfile(SOURCE_ROOT / "LICENSE", stage / "LICENSE")
            shutil.copyfile(
                SOURCE_ROOT / "THIRD-PARTY-NOTICES.md", stage / "THIRD-PARTY-NOTICES.md"
            )
            # Runtime files the core reads from RetroArch's system directory; no game data.
            system = stage / "system/spm-r8pe01"
            system.mkdir(parents=True)
            for name in SYSTEM_FILES:
                source = core.parent / name
                if source.is_dir():
                    shutil.copytree(source, system / name)
                else:
                    shutil.copy2(source, system / name)
            files = [CORE_NAME, "spm_libretro.info", "LICENSE", "THIRD-PARTY-NOTICES.md"]
            files += sorted(str(f.relative_to(stage)) for f in system.rglob("*") if f.is_file())
            diff = subprocess.check_output(
                ["git", "-C", str(SOURCE_ROOT), "diff", "--binary", "HEAD", "--", "."]
            )
            manifest = {
                "core_sha256": sha256(packaged_core),
                "source_build_key": json.loads((ROOT / "build-logs/android" / (args.build_record + ".json")).read_text())["key"],
                "source_git_head": git("rev-parse", "HEAD"),
                "source_dirty": bool(git("status", "--porcelain", "--untracked-files=no")),
                "source_tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
                "files": {name: sha256(stage / name) for name in files},
                "abi": "arm64-v8a",
                "development": True,
            }
            (stage / "manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )

            if output.exists():
                if any(output.iterdir()) and not known_output:
                    raise ValueError(
                        f"refusing to replace non-empty unknown output directory: {output}"
                    )
                if known_output and any(output.iterdir()):
                    backup = output.with_name(f".{output.name}.previous-{os.getpid()}")
                    if backup.exists():
                        raise ValueError(f"backup path already exists: {backup}")
                    output.rename(backup)
                else:
                    output.rmdir()
            try:
                stage.rename(output)
                stage = None
            except Exception:
                if backup and backup.exists() and not output.exists():
                    backup.rename(output)
                    backup = None
                raise
            if backup:
                shutil.rmtree(backup)
                backup = None
            log.write("Package completed successfully.\n")
            print(f"PASS: Android development package {output} (log: {log_path})")
            return 0
        except Exception as error:
            log.write("\nFAILURE\n")
            traceback.print_exc(file=log)
            if stage and stage.exists():
                shutil.rmtree(stage, ignore_errors=True)
            if backup and backup.exists() and not args.output.exists():
                backup.rename(args.output)
            print(f"FAIL: {error} (log: {log_path})", file=sys.stderr)
            return 1


if __name__ == "__main__":
    raise SystemExit(main())
