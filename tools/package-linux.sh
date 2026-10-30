#!/usr/bin/env bash
# Packs the Linux download: dist/NewSuperPaperBros-linux.tar.gz. Run on Linux or in WSL after tools/build-linux.sh
# (and --pal) and a Windows build (Play\ holds the shared assets).
# Same contents as package-spm.ps1's engine\, with the Linux programs; the disc is never copied in.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build=$HOME/spm-linux
version=v$(grep -oP '(?<=<Version>)[^<]+' "$root/launcher/NewSuperPaperBros.Launcher.csproj")
stage=$build/stage/NewSuperPaperBros
engine=$stage/engine
rm -rf "$build/stage"
mkdir -p "$engine/UserData/mods" "$engine/licenses" "$engine/game_files/sound"

for region in usa pal; do
    [[ -x $build/$region/build/WiiCompiled ]] || { echo "missing the $region build: run tools/build-linux.sh (and --pal)" >&2; exit 1; }
    cp "$build/$region/build/WiiCompiled" "$engine/nspb-engine-$region"
done
install -m 755 "$root/tools/starter/new-super-paper-bros.sh" "$stage/New Super Paper Bros"
cp "$build/usa/build/_deps/png-build/libpng16.so.16" "$engine/"   # the one library the game does not build in
cp -L "$build/sdl-shared/libSDL3.so.0" "$engine/"
cp "$build/usa/build/dsp_coef.bin" "$build/usa/build/cacert.pem" "$build/usa/build/initial_pipeline_cache.db" "$root/Play/portable.txt" "$engine/"
cp -r "$root/Play/startup" "$root/Play/wii_bootstrap" "$engine/"
echo "$version" > "$engine/version.txt"   # the built-in updater compares this with the latest release
# Built-in HQ music: the mod's brstm files that replaced the disc's own (Disc/sound_original keeps the originals).
for original in "$root"/Disc/sound_original/*.brstm; do
    hq=$root/Disc/DATA/files/sound/$(basename "$original")
    cmp -s "$hq" "$original" && { echo "$hq is the disc's own file, not the HQ mod's" >&2; exit 1; }
    cp "$hq" "$engine/game_files/sound/"
done
mkdir -p "$engine/UserData/texture_replacements"
cp -r "$root/Play/UserData/texture_replacements/SPM_HD_4x_BC3" "$engine/UserData/texture_replacements/"
cp -r "$root/Play/UserData/button_icons" "$engine/UserData/"
cp "$root/tools/CREDITS.txt" "$stage/CREDITS.txt"
cp "$root/wiicompiled/LICENSE" "$stage/LICENSE.txt"
mkdir -p "$stage/mod-sdk" && cp "$root/wiicompiled/runtime/include/spm_mod_api.h" "$root/tools/mod-sdk/example_mod.c" "$stage/mod-sdk/"   # code mods (README "Mods")
cp "$root/wiicompiled/THIRD-PARTY-NOTICES.md" "$engine/licenses/"
for dir in "$root"/wiicompiled/Dependencies/*/; do
    for f in "$dir"LICEN[CS]E* "$dir"COPYING* "$dir"NOTICE*; do
        [[ -f $f ]] && cp "$f" "$engine/licenses/$(basename "$dir")-$(basename "${f%.*}").txt"
    done
done
sed "s/{VERSION}/$version/" "$root/tools/package-README-linux.txt" >"$stage/README.txt"
# Files copied off /mnt/c come out executable: only the programs should be.
find "$stage" -type f -exec chmod 644 {} +
chmod 755 "$stage/New Super Paper Bros" "$engine"/nspb-engine-*

# Guard: no disc data in the download.
if find "$stage" -type d \( -name Disc -o -name DATA -o -name sys \) | grep -q .; then echo 'disc folders in the build' >&2; exit 1; fi

mkdir -p "$root/dist"
tar -C "$build/stage" -czf "$root/dist/NewSuperPaperBros-linux.tar.gz" NewSuperPaperBros
echo "DONE: dist/NewSuperPaperBros-linux.tar.gz ($version, $(du -sh "$root/dist/NewSuperPaperBros-linux.tar.gz" | cut -f1))"
