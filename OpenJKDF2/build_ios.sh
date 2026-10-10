#!/bin/sh
# iOS / iOS Simulator build.
#
#   ./build_ios.sh                  # arm64 device slice   -> build_ios/        + OpenJKDF2-iOS.app
#   IOS_PLATFORM=SIMULATOR ./build_ios.sh
#
# Rendering goes through ANGLE (Metal backend) so the engine keeps its GLSL ES
# 3.00 shaders -- see cmake_modules/build_angle.cmake for where ANGLE comes from
# and how to build it. Override the checkout with ANGLE_ROOT=/path/to/angle.
#
# Signing: ad-hoc by default, which the Simulator accepts. For a device, export
# IOS_CODESIGN_IDENTITY (e.g. "Apple Development: you@example.com (XXXXXXXXXX)"),
# IOS_BUNDLE_ID matching your provisioning profile, and IOS_ENTITLEMENTS.

export OPENJKDF2_RELEASE_COMMIT=$(git log -1 --format="%H")
export OPENJKDF2_RELEASE_COMMIT_SHORT=$(git rev-parse --short=8 HEAD)

# The slice is selected by picking a toolchain FILE -- see the comment at the top
# of cmake_modules/toolchain_ios.cmake for why it can't be a -D or an env var.
case "${IOS_PLATFORM:-OS}" in
    SIMULATOR|Simulator|simulator)
        BUILD_DIR=build_ios_sim
        TOOLCHAIN=toolchain_ios_sim.cmake
        ;;
    *)
        BUILD_DIR=build_ios
        TOOLCHAIN=toolchain_ios.cmake
        ;;
esac

# The host toolchain env the macOS build sets would otherwise drag macOS headers
# and the macOS deployment target into every ExternalProject sub-build.
unset SDKROOT MACOSX_DEPLOYMENT_TARGET CPLUS_INCLUDE_PATH C_INCLUDE_PATH CC CXX

mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR" || exit 1

cmake .. --toolchain "$(pwd)/../cmake_modules/$TOOLCHAIN" || exit 1

# ExternalProject sub-builds (SDL3, OpenAL, zlib, libpng) resolve during the
# first pass; the second pass picks up their now-existing imported libraries.
make -j"$(sysctl -n hw.ncpu)" openjkdf2-ios || make -j1 openjkdf2-ios || exit 1

case "${IOS_PLATFORM:-OS}" in
    SIMULATOR|Simulator|simulator) echo "Built ../OpenJKDF2-iOS-Simulator.app" ;;
    *)                             echo "Built ../OpenJKDF2-iOS.app" ;;
esac
