# ANGLE (Almost Native Graphics Layer Engine) as the GLES3 implementation.
#
# WHY: iOS only ships Apple's deprecated OpenGL ES 3.0 (and SDL3's UIKit backend
# drives it through EAGL, which has no EGL at all). Rather than port the engine's
# GLSL to Metal, we render through ANGLE's Metal backend -- so the exact same
# `#version 300 es` shaders in resource/shaders/ that Android and WASM run are
# what iOS runs too. ANGLE translates them to MSL at runtime.
#
# ANGLE is *not* built here: its build is gn/ninja/depot_tools and pulls ~12 GB.
# We consume a prebuilt checkout. Point ANGLE_ROOT at an ANGLE tree that has
# `include/` plus `out/<slice>/lib{EGL,GLESv2}.framework`, built with e.g.
#   gn gen out/ios --args='is_debug=false target_os="ios" target_cpu="arm64"
#       target_environment="device" ios_enable_code_signing=false
#       angle_enable_vulkan=false angle_enable_swiftshader=false'
#   autoninja -C out/ios libEGL libGLESv2
# (`target_environment="simulator"` + out/ios-sim for the Simulator slice.)

if(NOT ANGLE_ROOT)
    if(DEFINED ENV{ANGLE_ROOT})
        set(ANGLE_ROOT $ENV{ANGLE_ROOT})
    elseif(EXISTS ${PROJECT_SOURCE_DIR}/3rdparty/angle/include/EGL/egl.h)
        # Vendored-in-repo copy takes precedence over the out-of-tree default.
        set(ANGLE_ROOT ${PROJECT_SOURCE_DIR}/3rdparty/angle)
    else()
        set(ANGLE_ROOT $ENV{HOME}/workspace/Klepton/vendor)
    endif()
endif()
set(ANGLE_ROOT ${ANGLE_ROOT} CACHE PATH "Root of a prebuilt ANGLE checkout")

if(PLAT_IOS_SIMULATOR)
    set(ANGLE_SLICE_DEFAULT "ios-sim")
else()
    set(ANGLE_SLICE_DEFAULT "ios")
endif()
if(NOT ANGLE_SLICE)
    if(DEFINED ENV{ANGLE_SLICE})
        set(ANGLE_SLICE $ENV{ANGLE_SLICE})
    else()
        set(ANGLE_SLICE ${ANGLE_SLICE_DEFAULT})
    endif()
endif()
set(ANGLE_SLICE ${ANGLE_SLICE} CACHE STRING "Subdirectory of ANGLE_ROOT/out to link against")

set(ANGLE_INCLUDE_DIR ${ANGLE_ROOT}/include)
set(ANGLE_FRAMEWORK_DIR ${ANGLE_ROOT}/out/${ANGLE_SLICE})
set(ANGLE_EGL_FRAMEWORK ${ANGLE_FRAMEWORK_DIR}/libEGL.framework)
set(ANGLE_GLESV2_FRAMEWORK ${ANGLE_FRAMEWORK_DIR}/libGLESv2.framework)

if(NOT EXISTS ${ANGLE_INCLUDE_DIR}/EGL/egl.h)
    message(FATAL_ERROR "ANGLE headers not found at ${ANGLE_INCLUDE_DIR}. Set -DANGLE_ROOT=<angle checkout>.")
endif()
if(NOT EXISTS ${ANGLE_EGL_FRAMEWORK}/libEGL OR NOT EXISTS ${ANGLE_GLESV2_FRAMEWORK}/libGLESv2)
    message(FATAL_ERROR
        "ANGLE frameworks not found in ${ANGLE_FRAMEWORK_DIR}.\n"
        "Build them first (see the header of cmake_modules/build_angle.cmake), "
        "or set -DANGLE_SLICE=<out subdir>.")
endif()

message(STATUS "Using prebuilt ANGLE: ${ANGLE_FRAMEWORK_DIR}")

# Imported as frameworks so the linker records @rpath/libEGL.framework/libEGL,
# which is what these dylibs' install names already say; the app bundle gets an
# LC_RPATH of @executable_path/Frameworks and a copy of both (target_ios_all.cmake).
if(NOT TARGET ANGLE::EGL)
    add_library(ANGLE::EGL SHARED IMPORTED)
    set_target_properties(ANGLE::EGL PROPERTIES
        IMPORTED_LOCATION ${ANGLE_EGL_FRAMEWORK}/libEGL
        FRAMEWORK TRUE
        INTERFACE_INCLUDE_DIRECTORIES ${ANGLE_INCLUDE_DIR})
endif()
if(NOT TARGET ANGLE::GLESv2)
    add_library(ANGLE::GLESv2 SHARED IMPORTED)
    set_target_properties(ANGLE::GLESv2 PROPERTIES
        IMPORTED_LOCATION ${ANGLE_GLESV2_FRAMEWORK}/libGLESv2
        FRAMEWORK TRUE
        INTERFACE_INCLUDE_DIRECTORIES ${ANGLE_INCLUDE_DIR})
endif()

# ANGLE's public headers must win over any SDK GLES/EGL headers.
include_directories(BEFORE SYSTEM ${ANGLE_INCLUDE_DIR})
