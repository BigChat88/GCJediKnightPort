include(cmake_modules/target_ios_all.cmake)

macro(plat_initialize)
    if(PLAT_IOS_SIMULATOR)
        message( STATUS "Targeting iOS Simulator (arm64)" )
    else()
        message( STATUS "Targeting iOS (arm64)" )
    endif()

    set(BIN_NAME "openjkdf2-ios")

    add_definitions(-DARCH_64BIT)
    add_definitions(-DTARGET_IOS)
    # iOS is POSIX; the engine's `LINUX` define gates the generic
    # unistd/pwd/signal paths, same as it does for macOS and Android. MACOS is
    # deliberately NOT defined -- that one gates desktop-GL shader versions,
    # Carbon, and AppKit, none of which exist here.
    add_definitions(-DLINUX)

    include(cmake_modules/plat_feat_full_sdl2.cmake)

    # Same trims as Android: no auto-updater, no GameNetworkingSockets (protobuf
    # + GNS do not cross-compile cleanly to iOS and multiplayer is not the point
    # of this target), no PhysFS, and OpenAL is built from source rather than found.
    set(TARGET_USE_PHYSFS FALSE)
    set(TARGET_USE_CURL FALSE)
    set(TARGET_BUILD_TESTS FALSE)
    set(TARGET_FIND_OPENAL FALSE)
    set(TARGET_USE_GAMENETWORKINGSOCKETS FALSE)

    set(TARGET_IOS TRUE)

    # Link OpenAL Soft statically -- a loose .dylib inside an iOS .app has to be
    # separately signed and is rejected by the App Store; a static archive avoids
    # the whole question. (ANGLE stays dynamic; it has to be, it's prebuilt.)
    set(OPENAL_BUILD_STATIC TRUE)

    # Distinct bundle names per slice: the two builds have separate build dirs but
    # would otherwise overwrite each other's .app, and an iPhoneSimulator binary
    # installed to a device (or vice versa) fails as an opaque arch mismatch.
    if(PLAT_IOS_SIMULATOR)
        set(BUNDLE "${PROJECT_SOURCE_DIR}/OpenJKDF2-iOS-Simulator.app")
    else()
        set(BUNDLE "${PROJECT_SOURCE_DIR}/OpenJKDF2-iOS.app")
    endif()

    # -fno-builtin-wcslen: same trap the WASM target hit (see plat_wasm.cmake).
    # jk.c's _wcslen(const char16_t*) is a plain `for(len=0;str[len];len++)` loop,
    # and at -O2 LLVM's builtin-recognition pass pattern-matches that exact shape
    # as the "wcslen idiom" and rewrites it into a call to libc wcslen() -- which
    # on Darwin counts 4-byte wchar_t, not our 2-byte char16_t, so every string
    # table entry comes back at half its length ("Loading..." renders as "Loadi").
    # Detect a regression with: nm -u build_ios/CMakeFiles/sith_engine.dir/src/jk.c.o
    # | grep '^_wcslen$'  -- it must print nothing.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -std=c11 -O2 -fshort-wchar -fno-builtin-wcslen -Wno-unused-variable -Wno-parentheses -Wno-missing-braces -Werror=implicit-function-declaration")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fshort-wchar -fno-builtin-wcslen")
    add_link_options(-fshort-wchar)

    include(cmake_modules/build_angle.cmake)
endmacro()

macro(plat_specific_deps)
    plat_sdl2_deps()
endmacro()
