macro(plat_initialize)
    message( STATUS "Targeting Nintendo GameCube" )
    # Every malloc/free goes to gcHeap.c (dlmalloc over a fixed MEM1 region).
    add_link_options(-Wl,--wrap=malloc,--wrap=free,--wrap=realloc,--wrap=calloc,--wrap=memalign)

    # Port version: the VERSION file at the root of the GCJediKnight repo (one level
    # above OpenJKDF2), or GC_PORT_VERSION from the environment (CI), else "dev".
    set(GC_PORT_VERSION "dev")
    if(DEFINED ENV{GC_PORT_VERSION} AND NOT "$ENV{GC_PORT_VERSION}" STREQUAL "")
        set(GC_PORT_VERSION "$ENV{GC_PORT_VERSION}")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/../VERSION")
        file(STRINGS "${CMAKE_SOURCE_DIR}/../VERSION" GC_PORT_VERSION LIMIT_COUNT 1)
        string(STRIP "${GC_PORT_VERSION}" GC_PORT_VERSION)
    endif()
    add_compile_definitions(GC_PORT_VERSION="${GC_PORT_VERSION}")
    message(STATUS "GC port version: ${GC_PORT_VERSION}")

    # Optional: boot straight into a map (debug/testing), e.g.
    #   GC_AUTOBOOT_MAP=01narshadda.jkl ./build_gamecube.sh
    if(DEFINED ENV{GC_AUTOBOOT_MAP})
        add_compile_definitions(GC_AUTOBOOT_MAP="$ENV{GC_AUTOBOOT_MAP}")
        message(STATUS "GC autoboot map: $ENV{GC_AUTOBOOT_MAP}")
    endif()

    # Optional: start with cutscenes disabled (faster boot tests), e.g.
    #   GC_SKIP_CUTSCENES=1 ./build_gamecube.sh
    if(DEFINED ENV{GC_SKIP_CUTSCENES})
        add_compile_definitions(GC_SKIP_CUTSCENES)
        message(STATUS "GC: cutscenes disabled by default")
    endif()

    # Optional: diagnostic build that periodically logs the main thread's call stack
    # (see src/Platform/GameCube/gcTrace.c), e.g. GC_TRACE=1 ./build_gamecube.sh
    if(DEFINED ENV{GC_TRACE} AND NOT "$ENV{GC_TRACE}" STREQUAL "")
        add_compile_definitions(GC_TRACE)
        add_compile_options(-finstrument-functions
            -finstrument-functions-exclude-file-list=/opt/devkitpro,Platform/GameCube)
        message(STATUS "GC: call-stack tracing enabled")
    endif()

    # Optional: scripted controller presses for automated tests, e.g.
    #   GC_INPUT_SCRIPT="25000:A,27500:DOWN" (ms since boot : button)
    if(DEFINED ENV{GC_INPUT_SCRIPT} AND NOT "$ENV{GC_INPUT_SCRIPT}" STREQUAL "")
        add_compile_definitions(GC_INPUT_SCRIPT="$ENV{GC_INPUT_SCRIPT}")
        message(STATUS "GC: scripted input $ENV{GC_INPUT_SCRIPT}")
    endif()

    # Optional: log every GOB sub-file open (GOBOPEN lines) to record level access
    # order for packaging/gamecube/record_access.sh.
    if(DEFINED ENV{GC_LOG_GOB_OPENS} AND NOT "$ENV{GC_LOG_GOB_OPENS}" STREQUAL "")
        add_compile_definitions(GC_LOG_GOB_OPENS)
    endif()
    # Optional: memory diagnostics -- per-source-file allocation accounting and a
    # "carry" line on the stats panel (memory still held at a loading screen
    # compared with the first one), e.g. GC_MEM_TRACK=1.
    if(DEFINED ENV{GC_MEM_TRACK} AND NOT "$ENV{GC_MEM_TRACK}" STREQUAL "")
        add_compile_definitions(STDPLATFORM_ALLOC_TRACKING GC_MEM_TRACK)
        message(STATUS "GC: memory tracking enabled")
    endif()
    # Optional: load this memory card save slot when the main menu first shows
    # (reproducing a player's save), e.g. GC_AUTOLOAD_SLOT=2.
    if(DEFINED ENV{GC_AUTOLOAD_SLOT} AND NOT "$ENV{GC_AUTOLOAD_SLOT}" STREQUAL "")
        add_compile_definitions(GC_AUTOLOAD_SLOT=$ENV{GC_AUTOLOAD_SLOT})
    endif()
    # Optional: memory panel under the frame rate (diagnostic ISOs), GC_MEMHUD=1.
    if(DEFINED ENV{GC_MEMHUD} AND NOT "$ENV{GC_MEMHUD}" STREQUAL "")
        add_compile_definitions(GC_MEMHUD)
        message(STATUS "GC: memory panel")
    endif()
    # Optional: cross-check every ARAM read-cache hit against the disc.
    if(DEFINED ENV{GC_DVD_VERIFY} AND NOT "$ENV{GC_DVD_VERIFY}" STREQUAL "")
        add_compile_definitions(GC_DVD_VERIFY)
    endif()
    # Optional: on-screen startup log, SD card notice and cutscene timings (useful
    # to diagnose boot problems on real hardware).
    if(DEFINED ENV{GC_BOOT_CONSOLE} AND NOT "$ENV{GC_BOOT_CONSOLE}" STREQUAL "")
        add_compile_definitions(GC_BOOT_CONSOLE)
    endif()
    # Optional: boot the level named in dvd:/autoboot.txt (test harness).
    if(DEFINED ENV{GC_AUTOBOOT_FROM_DISC} AND NOT "$ENV{GC_AUTOBOOT_FROM_DISC}" STREQUAL "")
        add_compile_definitions(GC_AUTOBOOT_FROM_DISC)
    endif()
    # Optional: multiplayer test without menus: GC_MP_TEST=host hosts m2.jkl,
    # GC_MP_TEST=join joins the first game found on the network.
    if(DEFINED ENV{GC_MP_TEST} AND NOT "$ENV{GC_MP_TEST}" STREQUAL "")
        add_compile_definitions(GC_MP_TEST_$ENV{GC_MP_TEST})
        message(STATUS "GC: multiplayer test ($ENV{GC_MP_TEST})")
    endif()

    set(BIN_NAME "openjkdf2")

    add_definitions(-DPLAT_MISSING_WIN32)
    add_definitions(-DTARGET_GAMECUBE)
    add_definitions(-DTARGET_RETRO_HOMEBREW)
    add_definitions(-D_DEFAULT_SOURCE)

    # No SDL2, no desktop GL: software video codecs, POSIX filesystem (newlib
    # devoptabs), no blobs, multiplayer over the network adapter.
    set(TARGET_USE_GCSOCKETS TRUE)
    set(TARGET_USE_LIBSMACKER TRUE)
    set(TARGET_USE_LIBSMUSHER TRUE)
    set(TARGET_POSIX TRUE)
    set(TARGET_NO_BLOBS TRUE)
    set(TARGET_NO_MULTIPLAYER_MENUS FALSE)
    set(TARGET_GAMECUBE TRUE)

    # The port (src/Platform/GameCube) is built on libogc2, Extrems' fork of libogc
    # (packaging/gamecube/Dockerfile): SD cards through libdvm (FAT and exFAT), more
    # network adapters, libogc2's dlmalloc. The engine includes the platform's
    # headers by name (gcDvd.h, ...).
    if(NOT "${DKP_OGC_PLATFORM_LIBRARY}" STREQUAL "libogc2")
        message(FATAL_ERROR "The GameCube port needs libogc2 (DKP_OGC_PLATFORM_LIBRARY is "
                            "'${DKP_OGC_PLATFORM_LIBRARY}'): build with the image from packaging/gamecube/Dockerfile.")
    endif()
    # Our crash screen (gcCrash.c) in place of libogc2's.
    add_link_options(-Wl,--wrap=c_default_exceptionhandler)
    include_directories(${PROJECT_SOURCE_DIR}/src/Platform/GameCube)

    # GameCube.cmake already injects -mogc -mcpu=750 -meabi -mhard-float.
    add_compile_options(-Wall -Wno-unused-variable -Wno-parentheses -Wno-missing-braces)
    add_compile_options(-ffunction-sections -fdata-sections -fshort-wchar)
    # Engine code is full of type punning through casts (decompiled), keep the
    # optimizer from exploiting strict aliasing.
    add_compile_options(-fno-strict-aliasing)
    add_compile_options(-O2)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")
    add_link_options(-Wl,--gc-sections)
endmacro()

macro(plat_link_and_package)
    # (No -laesnd: gcAesnd.c is our own aesndlib, with the DSP mixer built in.)
    # libdvm mounts SD cards (FAT and exFAT drivers from libogc2's libfat).
    target_link_libraries(${BIN_NAME} PRIVATE -ldvm -lfat -lbba -lz -logc -lm)
    # zlib (devkitPro portlibs): compressed save games on the memory card.
    target_include_directories(sith_engine PRIVATE $ENV{DEVKITPRO}/portlibs/ppc/include)
    target_link_directories(${BIN_NAME} PRIVATE $ENV{DEVKITPRO}/portlibs/ppc/lib)
    target_link_libraries(sith_engine PRIVATE nlohmann_json::nlohmann_json)

    target_link_options(${BIN_NAME} PRIVATE -Wl,-Map,openjkdf2.map)

    # openjkdf2.elf -> openjkdf2.dol (bootable in Dolphin / Swiss / SD Media Launcher).
    ogc_create_dol(${BIN_NAME})
endmacro()

