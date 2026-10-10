# CMake toolchain file for the Nintendo GameCube (devkitPPC + libogc).
#
# Requires devkitPro with the gamecube-dev group; the easiest way to get it is the
# Docker image in packaging/gamecube (see build_gamecube.sh).

if(NOT DEFINED ENV{DEVKITPRO})
    set(ENV{DEVKITPRO} /opt/devkitpro)
endif()

include($ENV{DEVKITPRO}/cmake/GameCube.cmake)

# Force the cache entry so the `set(PLAT_GAMECUBE FALSE CACHE BOOL ...)` default in
# CMakeLists.txt doesn't clobber it.
set(PLAT_GAMECUBE TRUE CACHE BOOL "Nintendo GameCube (libogc)" FORCE)

message(STATUS "Nintendo GameCube (devkitPPC/libogc) toolchain invoked")
