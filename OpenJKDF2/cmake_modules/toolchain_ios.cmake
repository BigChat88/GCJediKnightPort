# CMake toolchain file: iOS, arm64 device slice.
# (The Simulator slice is cmake_modules/toolchain_ios_sim.cmake.)
#
# NOTE: this is deliberately a plain CMAKE_SYSTEM_NAME=iOS toolchain rather than
# one of the third-party ios.toolchain.cmake files -- everything OpenJKDF2 needs
# (SDL3, OpenAL Soft, zlib, libpng) builds fine with CMake's own iOS support, and
# the ExternalProject_Add()s in cmake_modules/ all forward
# `--toolchain ${CMAKE_TOOLCHAIN_FILE}`, so the sub-builds inherit this verbatim.

# The slice is chosen by WHICH TOOLCHAIN FILE you point at, not by a variable:
# toolchain_ios.cmake = device, toolchain_ios_sim.cmake = Simulator (it sets
# IOS_PLATFORM and then includes this file).
#
# This matters because every ExternalProject_Add() in cmake_modules/ forwards
# `--toolchain ${CMAKE_TOOLCHAIN_FILE}` to a sub-build with a *fresh, empty*
# cache and no guarantee of inheriting the parent's environment. A toolchain that
# read the slice from -D or from an env var would silently configure SDL3/OpenAL
# for the device while the top-level build targeted the Simulator, and the link
# would fail with "built for 'iOS'" object files. The file path always propagates.
if(NOT DEFINED IOS_PLATFORM)
    set(IOS_PLATFORM "OS")
endif()
string(TOUPPER "${IOS_PLATFORM}" IOS_PLATFORM)

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)

if(DEFINED ENV{IOS_DEPLOYMENT_TARGET})
    set(CMAKE_OSX_DEPLOYMENT_TARGET $ENV{IOS_DEPLOYMENT_TARGET} CACHE STRING "" FORCE)
else()
    # 18.0 because that is the minos stamped into the prebuilt ANGLE frameworks we
    # embed (see cmake_modules/build_angle.cmake); dyld refuses to load a dylib whose
    # minos is newer than the running OS. Rebuild ANGLE lower and override this if needed.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "18.0" CACHE STRING "" FORCE)
endif()

if(IOS_PLATFORM STREQUAL "SIMULATOR")
    set(CMAKE_OSX_SYSROOT "iphonesimulator" CACHE STRING "" FORCE)
    set(PLAT_IOS_SIMULATOR TRUE CACHE BOOL "iOS Simulator slice" FORCE)
else()
    set(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "" FORCE)
    set(PLAT_IOS_SIMULATOR FALSE CACHE BOOL "iOS Simulator slice" FORCE)
endif()

# We assemble (and sign) the .app ourselves in target_ios_all.cmake, so none of
# the sub-projects need to try -- and none of them have a signing identity.
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO" CACHE STRING "" FORCE)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO" CACHE STRING "" FORCE)

# The host is macOS, so host tools (python3, protoc, ...) must not be searched
# for inside the iOS sysroot, but libraries/headers must.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(PLAT_IOS TRUE CACHE BOOL "iOS target" FORCE)

message(STATUS "iOS toolchain invoked (IOS_PLATFORM=${IOS_PLATFORM}, min ${CMAKE_OSX_DEPLOYMENT_TARGET})")
