#!/bin/bash
# Build the standalone Android app (APK) from `pipeline.py build` and install it with adb.
# Usage: app-deploy.sh [--no-build]
#   Needs JAVA_HOME (JDK 17), ANDROID_HOME (SDK) and ANDROID_NDK_ROOT; ADB_SERIAL picks a device.
#   APP_ARCH=armv82-a uses the --arch armv8.2-a build. NO_INSTALL=1 only copies the APK to dist/.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; APP=$ROOT/wiicompiled/android; ID=com.wiicompiled.spm
ADB=(adb ${ADB_SERIAL:+-s "$ADB_SERIAL"})
APK=$APP/app/build/outputs/apk/release/app-release.apk
LOG=$ROOT/build-logs/android/app-deploy.log; mkdir -p "$(dirname "$LOG")"; : > "$LOG"
if [ "$1" != "--no-build" ]; then
  mkdir -p "$APP/native/jniLibs/arm64-v8a" "$APP/native/assets/runtime"
  "$ANDROID_NDK_ROOT"/toolchains/llvm/prebuilt/*/bin/llvm-strip --strip-unneeded -o "$APP/native/jniLibs/arm64-v8a/libmain.so" \
    "$ROOT/wiicompiled/native-build/android-app${APP_ARCH:+-$APP_ARCH}/libmain.so"
  rsync -a --delete "$ROOT/out/android/system/spm-r8pe01/" "$APP/native/assets/runtime/"
  (cd "$APP" && ./gradlew assembleRelease -q >>"$LOG" 2>&1) || { grep -iE "error|what went wrong" -A3 "$LOG" | head -12; exit 1; }
  echo "APK $(du -h "$APK" | cut -f1)"
fi
if [ -n "$NO_INSTALL" ]; then
  mkdir -p "$ROOT/dist"; OUT="$ROOT/dist/spm-standalone${APP_ARCH:+-$APP_ARCH}.apk"
  cp "$APK" "$OUT"; echo "PASS app-package: $OUT"; exit 0
fi
"${ADB[@]}" install -r "$APK" >>"$LOG" 2>&1 || { tail -3 "$LOG"; exit 1; }
"${ADB[@]}" shell am start -n $ID/com.wither.nsmbw.LauncherActivity >>"$LOG" 2>&1
echo "PASS app-deploy: installed and started $ID (log $LOG)"
