#!/bin/bash
# Iteration loop: rebuild the GameCube DOL in Docker, then boot it in headless
# Dolphin. Usage: ./dev.sh [seconds] [--no-build]
#
# Builds in OpenJKDF2/build_gamecube_dev, never in build_gamecube (the release
# build): the dev options below are baked in at configure time, and a release
# built from a folder that once had them booted straight into a test level.
#
# Images: openjkdf2-gamecube (packaging/gamecube/Dockerfile, libogc2) builds;
# openjkdf2-dolphin (packaging/gamecube/test, Dolphin 2609) runs. That Dolphin
# only boots disc images, so the DOL is packed into dev.iso first, with the Jedi
# Knight data that build.sh extracted from gamedata/ (output/work/jk) --
# with the apploader of the release build (OpenJKDF2/build_gamecube).
ROOT="$(cd "$(dirname "$0")" && pwd)"
WIN_ROOT="$(cygpath -w "$ROOT")"
SECS="${1:-40}"
BDIR=build_gamecube_dev
if [ "$2" != "--no-build" ]; then
    # Dev options are read at configure time: GC_SKIP_CUTSCENES=1, GC_AUTOBOOT_MAP=xx.jkl
    MSYS_NO_PATHCONV=1 docker run --rm -e GC_SKIP_CUTSCENES -e GC_AUTOBOOT_MAP -e GC_TRACE -e GC_INPUT_SCRIPT -e GC_LOG_GOB_OPENS -e GC_AUTOBOOT_FROM_DISC -e GC_DVD_VERIFY -e GC_BOOT_CONSOLE -e GC_MEM_TRACK -e GC_MP_TEST -e GC_MEMHUD -e BDIR=$BDIR \
        -v "$WIN_ROOT\OpenJKDF2:/src" -w /src openjkdf2-gamecube \
        bash -c 'mkdir -p $BDIR && cd $BDIR && { [ -f CMakeCache.txt ] || EXPERIMENTAL_FIXED_POINT=0 DEBUG_QOL_CHEATS=0 cmake .. --toolchain "$PWD/../cmake_modules/toolchain_gamecube.cmake" > /dev/null; } && cmake -U "GC_*" . > /dev/null && make -j8 openjkdf2 2>&1' > "$ROOT/build.log" 2>&1 \
        || { grep -E "error|Error" "$ROOT/build.log" | head -30; exit 1; }
fi
python "$ROOT/OpenJKDF2/packaging/gamecube/make_gcm.py" "$ROOT/OpenJKDF2/$BDIR/openjkdf2.dol" "$ROOT/output/work/jk" \
    -o "$ROOT/dev.iso" --apploader "$ROOT/OpenJKDF2/build_gamecube/apploader.img" > /dev/null || exit 1
MSYS_NO_PATHCONV=1 docker run --rm --shm-size=1g -e DOLPHIN_ARGS -v "$WIN_ROOT:/work" openjkdf2-dolphin \
    /work/dev.iso /work/dev.iso "$SECS" > "$ROOT/test.log" 2>&1
sed 's/^[0-9:]* Core\/HW\/EXI\/EXI_DeviceIPL.cpp:[0-9]* N\[OSREPORT\]: //' "$ROOT/test.log"
