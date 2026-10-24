#!/bin/bash
# Wrap the macOS build (native-build/macos-diagnostic/WiiCompiled, from tools/android/host-diagnostic.py build)
# into dist/Super Paper Mario.app. The runtime finds its data files beside the executable; Config.toml,
# saves and logs live in ~/Library/Application Support/WiiCompiled (paths.dvd_root = the extracted game).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd); BUILD=$ROOT/wiicompiled/native-build/macos-diagnostic
APP="$ROOT/dist/Super Paper Mario.app"; rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/WiiCompiled" "$APP/Contents/MacOS/"
cp -R "$BUILD/cacert.pem" "$BUILD/dsp_coef.bin" "$BUILD/initial_pipeline_cache.db" "$BUILD/wii_bootstrap" "$APP/Contents/MacOS/"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Super Paper Mario</string>
  <key>CFBundleIdentifier</key><string>local.wiicompiled.spm</string>
  <key>CFBundleExecutable</key><string>WiiCompiled</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <!-- Without these macOS naps the game when it is launched from Finder: about one frame every 4.7 s. -->
  <key>LSAppNapIsDisabled</key><true/>
  <key>NSAppSleepDisabled</key><true/>
</dict></plist>
PLIST
codesign --force --deep -s - "$APP" >/dev/null 2>&1
echo "PASS macOS app: $APP ($(du -sh "$APP" | cut -f1))"
