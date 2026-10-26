#!/bin/bash
# Build OpenJKDF2 for the Nintendo GameCube (devkitPPC + libogc2).
#
# Needs devkitPro with the GameCube toolchain and libogc2. Without them, run it in
# the Docker image from packaging/gamecube:
#
#   docker build -t openjkdf2-gamecube packaging/gamecube
#   docker run --rm -v "$PWD:/src" -w /src openjkdf2-gamecube ./build_gamecube.sh
#
# Output: build_gamecube/openjkdf2.dol (+ .elf and .map for symbolication).

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"

export OPENJKDF2_RELEASE_COMMIT=$(git log -1 --format="%H" 2>/dev/null)
export OPENJKDF2_RELEASE_COMMIT_SHORT=$(git rev-parse --short=8 HEAD 2>/dev/null)

mkdir -p build_gamecube && pushd build_gamecube

EXPERIMENTAL_FIXED_POINT=0 DEBUG_QOL_CHEATS=0 \
    cmake .. --toolchain $(pwd)/../cmake_modules/toolchain_gamecube.cmake &&
make -j $(nproc 2>/dev/null || echo 4) openjkdf2 &&
bash ../packaging/gamecube/apploader/build.sh $(pwd)/apploader.img
RC=$?

if [ $RC -eq 0 ]; then
    echo ""
    echo "Built artifacts:"
    ls -lh openjkdf2.dol openjkdf2.elf apploader.img 2>/dev/null
fi

popd
exit $RC
