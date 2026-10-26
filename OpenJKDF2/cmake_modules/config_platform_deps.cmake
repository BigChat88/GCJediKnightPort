if(TARGET_NO_BLOBS)
    add_definitions(-DLINUX_TMP)
    add_definitions(-DNO_JK_MMAP)
endif()

if (TARGET_NO_MULTIPLAYER_MENUS)
    add_definitions(-DTARGET_NO_MULTIPLAYER_MENUS)
endif()

# Enables all force powers by default, useful for debugging.
if(DEBUG_QOL_CHEATS)
    add_definitions(-DDEBUG_QOL_CHEATS)
endif()

# Enables Cxx compiling for fixed point templates
if(EXPERIMENTAL_FIXED_POINT)
    add_definitions(-DEXPERIMENTAL_FIXED_POINT)
endif()

# Multiplayer: TCP over the network adapter (lwIP)
if(TARGET_USE_GCSOCKETS)
    file(GLOB TARGET_GCSOCK_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/Networking/GameCube/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_GCSOCK_SRCS})

    add_definitions(-DPLATFORM_GCSOCKETS)
endif()

if(TARGET_USE_LIBSMACKER)
    list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/external/libsmacker/smacker.c ${PROJECT_SOURCE_DIR}/src/external/libsmacker/smk_bitstream.c ${PROJECT_SOURCE_DIR}/src/external/libsmacker/smk_hufftree.c)
endif()

if(TARGET_USE_LIBSMUSHER)
    list(APPEND ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/external/libsmusher/src/smush.c ${PROJECT_SOURCE_DIR}/src/external/libsmusher/src/codec48.c ${PROJECT_SOURCE_DIR}/src/external/libsmusher/src/codec47.c)
endif()

add_compile_definitions(STDSOUND_GAMECUBE)

if(TARGET_POSIX)
    file(GLOB TARGET_POSIX_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/Posix/*.c)
    list(APPEND ENGINE_SOURCE_FILES ${TARGET_POSIX_SRCS})

    add_definitions(-DPLATFORM_POSIX)
endif()

file(GLOB TARGET_GAMECUBE_SRCS ${PROJECT_SOURCE_DIR}/src/Platform/GameCube/*.c)
list(APPEND ENGINE_SOURCE_FILES ${TARGET_GAMECUBE_SRCS})
# No hardware-accelerated dev console (it relies on the SDL2/GL UI text path).
list(REMOVE_ITEM ENGINE_SOURCE_FILES ${PROJECT_SOURCE_DIR}/src/Main/jkQuakeConsole.c)

file(GLOB EMBEDDED_RESOURCES ${PROJECT_SOURCE_DIR}/resource/ui/*)
