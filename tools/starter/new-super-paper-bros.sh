#!/bin/sh
# The Linux download's top-level "New Super Paper Bros": runs the engine for the disc's region. On first start the
# game sets itself up from a disc image put in this folder; settings, mods and updates are in its Options menu.
cd "$(dirname "$0")/engine" || exit 1
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ "$(head -c 6 ../Disc/DATA/sys/boot.bin 2>/dev/null)" = R8PP01 ]; then
    exec ./nspb-engine-pal
fi
exec ./nspb-engine-usa
