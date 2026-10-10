#!/bin/bash
# Iteration loop: rebuild the GameCube DOL in Docker, then boot it in headless
# Dolphin with openjkdf2.iso as the disc. Usage: ./dev.sh [seconds] [--no-build]
ROOT="$(cd "$(dirname "$0")" && pwd)"
WIN_ROOT="$(cygpath -w "$ROOT")"
SECS="${1:-40}"
if [ "$2" != "--no-build" ]; then
    # Dev options are read at configure time: GC_SKIP_CUTSCENES=1, GC_AUTOBOOT_MAP=xx.jkl
    MSYS_NO_PATHCONV=1 docker run --rm -e GC_SKIP_CUTSCENES -e GC_AUTOBOOT_MAP -e GC_TRACE -e GC_INPUT_SCRIPT -e GC_LOG_GOB_OPENS -e GC_AUTOBOOT_FROM_DISC -e GC_DVD_VERIFY -e GC_BOOT_CONSOLE -e GC_MEM_TRACK -e GC_MP_TEST -e GC_MEMHUD \
        -v "$WIN_ROOT\OpenJKDF2:/src" -w /src openjkdf2-gamecube \
        bash -c "cd build_gamecube && cmake -U GC_* . > /dev/null && make -j8 openjkdf2 2>&1" > "$ROOT/build.log" 2>&1 \
        || { grep -E "error|Error" "$ROOT/build.log" | head -30; exit 1; }
fi
MSYS_NO_PATHCONV=1 docker run --rm --shm-size=1g -e DOLPHIN_ARGS -v "$WIN_ROOT:/work" openjkdf2-dolphin \
    /work/OpenJKDF2/build_gamecube/openjkdf2.dol /work/openjkdf2.iso "$SECS" > "$ROOT/test.log" 2>&1
sed 's/^[0-9:]* Core\/HW\/EXI\/EXI_DeviceIPL.cpp:[0-9]* N\[OSREPORT\]: //' "$ROOT/test.log"
