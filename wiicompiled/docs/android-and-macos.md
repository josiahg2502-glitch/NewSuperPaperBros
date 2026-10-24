# Super Paper Mario on Android and macOS

Builds of the Super Paper Mario port for Android (a libretro core for RetroArch, and a standalone
app) and for macOS. All of them translate your own USA disc, **R8PE01 revision 0**; no game data
is included or committed.

## Common steps

Requirements: Python 3 with `cryptography`, CMake and Ninja, and the .NET 8 SDK at
`toolchain/dotnet/dotnet` (or pass `--dotnet`). Android builds also need the Android NDK
(r29 tested) in `ANDROID_NDK_ROOT`.

The scripts in `tools/android/` run in stages. Each stage records its input hashes in
`build-logs/` and is skipped while nothing changed. A failing stage prints its log path.

```bash
python3 tools/android/pipeline.py prepare-inputs --disc /path/to/R8PE01.wbfs
```

```bash
python3 tools/android/pipeline.py translator
```

```bash
python3 tools/android/pipeline.py translate
```

`prepare-inputs` extracts the disc to `Disc/DATA`, checks the DOL hash, prelinks the REL and
writes `game/android-stock.yml` (stock audio). `translator` builds the translator and `translate`
writes the translated code to `wiicompiled/generated/`, which every target below builds from.

Dawn (WebGPU) comes from Aurora's pinned prebuilt package and is downloaded during configure.
To build offline, pass a local copy of the same archive with `--dawn` (its SHA-256 is checked).

## Android: libretro core

Needs an ARM64 device with Vulkan and RetroArch (aarch64) using the **vulkan** video driver.

```bash
python3 tools/android/pipeline.py all --core
```

```bash
python3 tools/android/package.py
```

The first command configures, builds and verifies `wiicompiled/native-build/android/spm_libretro.so`.
`verify-binary.py` checks its ABI, exports, dependencies and 16 KB segment alignment.
`package.py` writes the installable package to `out/android/`: the stripped
`spm_libretro_android.so`, `spm_libretro.info`, licenses, a hash manifest and
`system/spm-r8pe01/` (DSP ROM, CA bundle and Wii bootstrap files the runtime reads).

`--arch armv8.2-a` builds for devices with ARMv8.2-A in a separate tree (the default floor is
ARMv8-A). Package that build with `package.py --core wiicompiled/native-build/android-armv82-a/spm_libretro.so --build-record core-build-armv82-a`.

Install on the device:

1. Copy `system/spm-r8pe01/` into RetroArch's system directory (**Settings → Directory →
   System/BIOS**).
2. **Load Core → Install or Restore a Core** and pick `spm_libretro_android.so`.
3. Copy `spm_libretro.info` into the **Core Info** directory so RetroArch shows the core's name and
   only offers `.wbfs` / `boot.bin` content. If the list still shows the file name, quit RetroArch
   and delete `core_info.cache` from that directory.
4. **Load Content** and pick the `.wbfs`. An extracted disc also works: load its `sys/boot.bin`.

### What the core does

- **Disc images.** A `.wbfs` is checked to be R8PE01 revision 0 and unpacked once into
  `<system>/spm-r8pe01/disc/` (`frontend/disc_image.cpp`, the C++ counterpart of
  `tools/extract_wbfs.py`). A marker file written last makes an interrupted unpack restart.
  Delete the folder to free the space; the next launch unpacks again.
- **Preparing screen.** While the disc unpacks, `frontend/prepare_screen.cpp` draws an animated
  progress screen with a completion chime, on a private Dawn device that is released before the
  game starts. `make-prepare-text.py` regenerates its committed text atlas.
- **Video.** Aurora renders the game windowless on its own Vulkan Dawn device. `frontend/bridge.cpp`
  hands each frame to RetroArch through an AHardwareBuffer and sync FDs, using RetroArch's Vulkan
  context negotiation (v2). Both devices must be the same GPU.
- **Frames.** Each `retro_run` advances the guest by one VI retrace and suspends it in the
  scheduler's idle loop. RetroArch owns pacing; input is latched as the frame resumes, so the game
  sees it in the same frame.
- **Audio.** Guest PCM is queued, resampled to 48 kHz and drained every frame. The core reads
  RetroArch's audio buffer status for its timing log.
- **Input.** The pad acts as a sideways Wii Remote with the desktop build's rules. Bottom face
  button = 2 (jump), left/right face = 1, top face = A, left stick or D-pad = move, Start = +,
  Select or L1 = -, R1/R2 = shake, L2 = B, L3 = HOME. Touch or the right stick aims the pointer;
  in the arcade minigames the left stick also tilts.
- **Lifecycle.** Content can be closed and loaded again, and the Vulkan context can be lost and
  recreated (app switching) without restarting RetroArch.
- **Performance.** The game thread is kept off the slowest CPU cluster on big.LITTLE devices.
  Every 600 frames the log gets an `SPM perf` line (fps, game speed, time per phase, audio buffer).

### Save states

RetroArch's save and load state work once the game is running. A state holds the whole guest (memory, threads,
HLE, audio and GX state) and loads only into the same core build. It can be loaded in a fresh
session, including auto-load at start. Netplay, run-ahead and rewind are not supported.

The translator makes the functions in `projects/spm/resumable_functions.inc` resumable at their call
sites, so a loaded state can rebuild each guest thread's native stack. Saving is refused when a
thread's call chain goes through a function not on that list; the log names the function.

### Core options

| Option | Values | Effect |
| --- | --- | --- |
| `spm_frame_rate` | `60`, `30` | `30` runs two VI retraces per frame, keeping full game speed on devices that can't hold 60 fps. Needs a restart. |
| `spm_fast_cores` | `enabled`, `disabled` | Keep the game thread on the fast CPU cores. |
| `spm_video_contract` | `ahb-gpu`, `ahb-cpu-wait` | `ahb-cpu-wait` waits on the frame's sync FDs on the CPU instead of in GPU semaphores. Needs a restart. |
| `spm_prepare_resolution` | `640x480`, `960x720`, `1280x960` | Resolution of the Preparing screen. |
| `spm_frame_stats` | `enabled`, `disabled` | Frame-timing panel on the Preparing screen. |

## Android: standalone app

The same runtime as a regular app (SDL, no RetroArch), in `wiicompiled/android`. Build the native
library, then the APK. The APK bundles the runtime files from `out/android/system/`, so run
`package.py` once first. Building the APK needs JDK 17 in `JAVA_HOME` and the Android SDK in
`ANDROID_HOME`.

```bash
python3 tools/android/pipeline.py all
```

```bash
tools/android/app-deploy.sh
```

`app-deploy.sh` builds `app-release.apk`, installs it with `adb` (`ADB_SERIAL` picks a device) and
starts it. `NO_INSTALL=1` copies the APK to `dist/` instead, and `APP_ARCH=armv82-a` uses an
`--arch armv8.2-a` build. On first start, the app asks for the game: import a disc extracted with
Dolphin, as the launcher describes.

## macOS

`host-diagnostic.py` builds the runtime for macOS ARM64 (Metal) from the same translation, as
`wiicompiled/native-build/macos-diagnostic/WiiCompiled`. It reuses dependency sources from an
Android build when there is one.

```bash
python3 tools/android/host-diagnostic.py configure
```

```bash
python3 tools/android/host-diagnostic.py build
```

### Native game

`WiiCompiled` is the normal desktop runtime. Point `paths.dvd_root` in
`~/Library/Application Support/WiiCompiled/Config.toml` at `Disc/DATA` and start it from the build
folder. Or wrap it as an app:

```bash
tools/package-macos-app.sh
```

This writes `dist/Super Paper Mario.app`, which keeps its config, saves and logs in the same
Application Support folder.

### Libretro runtime path

The same executable runs the libretro core's runtime path on macOS, windowless and without
RetroArch: the frontend boot, frame stepping, audio sink, input adapter and lifecycle. Use it to
check frontend changes without a device.

```bash
python3 tools/android/host-diagnostic.py run --frames 3600 --output build-logs/host/run-1
```

The output folder gets `runtime.log` and `result.json`. `--cycles N` loads and unloads the
content N times in one process. Environment variables add more:
`SPM_FRONTEND_INPUT="3000:TWO:6,3200:DOWN+A:10"` holds buttons from a frame for some frames, and
`SPM_FRAME_STEP=2` runs at half rate. `SPM_STATE_AT="3000:4000"` captures a state after frame 3000
and restores it after frame 4000. `SPM_STATE_SAVE="4000:/path"` and `SPM_STATE_LOAD="60:/path:4000"`
save and load a state file across runs.
