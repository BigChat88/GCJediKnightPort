# CMake toolchain file: iOS Simulator (arm64).
#
# A separate file rather than a -D flag so that the ExternalProject sub-builds,
# which only inherit CMAKE_TOOLCHAIN_FILE's path, land on the same slice. See the
# comment at the top of toolchain_ios.cmake.
set(IOS_PLATFORM "SIMULATOR")
include("${CMAKE_CURRENT_LIST_DIR}/toolchain_ios.cmake")
