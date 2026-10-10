# iOS link + .app assembly.
#
# Unlike the macOS target there is no Xcode project here: we build with the
# plain Makefile generator and lay the bundle out by hand in a POST_BUILD step,
# exactly like postcompile_macos() does. The only iOS-specific extras are the
# embedded ANGLE frameworks (+ the LC_RPATH that finds them) and the codesign
# pass, which iOS requires even for the Simulator.

function(ios_target_add_standard_deps target_name)
    if(NOT TARGET_IOS)
        return()
    endif()

    # SDL3 is consumed through a hand-rolled IMPORTED target (build_sdl.cmake), so
    # its link interface is not inherited -- every system framework SDL3's UIKit,
    # CoreAudio, CoreMotion, GameController and HIDAPI backends reference has to be
    # named here or the final link fails with hundreds of undefined ObjC symbols.
    foreach(fw
            UIKit Foundation CoreGraphics QuartzCore Metal
            AudioToolbox CoreAudio AVFoundation CoreMedia CoreVideo
            CoreMotion CoreBluetooth GameController ImageIO)
        target_link_libraries(${target_name} PRIVATE "-framework ${fw}")
    endforeach()

    # NOTE: OpenGLES.framework is deliberately absent. SDL3 is built with
    # SDL_OPENGLES=OFF here (build_sdl.cmake) precisely so that nothing on the
    # link line can capture the gl* symbols away from ANGLE's libGLESv2.

    # Weakly linked by SDL3 (absent on older/reduced platforms).
    target_link_libraries(${target_name} PRIVATE "-weak_framework CoreHaptics")
    target_link_libraries(${target_name} PRIVATE "-weak_framework UniformTypeIdentifiers")

    target_link_libraries(${target_name} PRIVATE iconv)

    # ANGLE's install names are @rpath-relative; the frameworks live in the bundle.
    target_link_options(${target_name} PRIVATE "-Wl,-rpath,@executable_path/Frameworks")
endfunction()

macro(postcompile_ios)
    if(PLAT_IOS_SIMULATOR)
        set(IOS_PLATFORM_NAME "iPhoneSimulator")
    else()
        set(IOS_PLATFORM_NAME "iPhoneOS")
    endif()

    # Ad-hoc ("-") is enough for the Simulator and for a locally re-signed
    # sideload; pass a real identity for a device build that dyld/AMFI will accept.
    if(DEFINED ENV{IOS_CODESIGN_IDENTITY})
        set(IOS_CODESIGN_IDENTITY $ENV{IOS_CODESIGN_IDENTITY})
    else()
        set(IOS_CODESIGN_IDENTITY "-")
    endif()

    if(DEFINED ENV{IOS_BUNDLE_ID})
        set(IOS_BUNDLE_ID $ENV{IOS_BUNDLE_ID})
    else()
        set(IOS_BUNDLE_ID "org.openjkdf2.openjkdf2")
    endif()

    # Entitlements are OPT-IN, and deliberately not applied by default: an ad-hoc
    # ("-") signature that carries entitlements is rejected at launch
    # ("SIGKILL (Code Signature Invalid)" / "Taskgated Invalid Signature"), because
    # entitlements have to be backed by a provisioning profile. Unsigned-entitlement
    # ad-hoc bundles run fine on the Simulator.
    #
    # A device build needs all three of IOS_CODESIGN_IDENTITY, IOS_TEAM_ID and
    # IOS_PROVISIONING_PROFILE. Setting IOS_TEAM_ID generates the entitlements from
    # packaging/ios/OpenJKDF2.entitlements.in; IOS_ENTITLEMENTS still overrides it
    # with a hand-written file if you need extra capabilities.
    if(DEFINED ENV{IOS_ENTITLEMENTS})
        set(IOS_ENTITLEMENTS_FLAG --entitlements $ENV{IOS_ENTITLEMENTS})
    elseif(DEFINED ENV{IOS_TEAM_ID})
        set(IOS_TEAM_ID $ENV{IOS_TEAM_ID})
        configure_file(${PROJECT_SOURCE_DIR}/packaging/ios/OpenJKDF2.entitlements.in
                       ${CMAKE_CURRENT_BINARY_DIR}/OpenJKDF2.entitlements @ONLY)
        set(IOS_ENTITLEMENTS_FLAG --entitlements ${CMAKE_CURRENT_BINARY_DIR}/OpenJKDF2.entitlements)
    else()
        set(IOS_ENTITLEMENTS_FLAG)
    endif()

    # The profile has to be INSIDE the bundle as embedded.mobileprovision before the
    # bundle is signed -- installd validates the signed entitlements against it, and
    # AMFI refuses to launch a device build that has none.
    if(DEFINED ENV{IOS_PROVISIONING_PROFILE})
        set(IOS_EMBED_PROFILE_CMD
            COMMAND ${CMAKE_COMMAND} -E copy $ENV{IOS_PROVISIONING_PROFILE} ${BUNDLE}/embedded.mobileprovision)
    else()
        set(IOS_EMBED_PROFILE_CMD)
    endif()

    # CFBundleSupportedPlatforms/MinimumOSVersion differ per slice, so the plist is
    # a template rather than a static file.
    configure_file(${PROJECT_SOURCE_DIR}/packaging/ios/Info.plist.in
                   ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

    add_custom_command(TARGET ${BIN_NAME}
        POST_BUILD
        COMMAND dsymutil ${CMAKE_CURRENT_BINARY_DIR}/${BIN_NAME} -o ${CMAKE_CURRENT_BINARY_DIR}/${BIN_NAME}.dsym
        COMMAND rm -rf ${BUNDLE}
        # iOS bundles are FLAT -- no Contents/MacOS, everything sits at the top level.
        COMMAND mkdir -p ${BUNDLE}/Frameworks
        COMMAND cp ${CMAKE_CURRENT_BINARY_DIR}/${BIN_NAME} ${BUNDLE}/${BIN_NAME}
        COMMAND cp ${CMAKE_CURRENT_BINARY_DIR}/Info.plist ${BUNDLE}/Info.plist
        COMMAND cp ${PROJECT_SOURCE_DIR}/packaging/icon-256.png ${BUNDLE}/AppIcon.png

        # ANGLE. Both frameworks ship: the app links libGLESv2 for the gl* entry
        # points and libEGL for context/surface management, and libEGL resolves
        # libGLESv2 through @rpath at load time.
        COMMAND cp -R ${ANGLE_EGL_FRAMEWORK} ${BUNDLE}/Frameworks/
        COMMAND cp -R ${ANGLE_GLESV2_FRAMEWORK} ${BUNDLE}/Frameworks/
        COMMAND chmod -R u+w ${BUNDLE}/Frameworks

        COMMAND cp -r ${PROJECT_SOURCE_DIR}/resource ${BUNDLE}/resource

        ${IOS_EMBED_PROFILE_CMD}

        # Sign inside-out: embedded code first, then the bundle.
        COMMAND codesign --force --sign "${IOS_CODESIGN_IDENTITY}" --timestamp=none ${BUNDLE}/Frameworks/libEGL.framework
        COMMAND codesign --force --sign "${IOS_CODESIGN_IDENTITY}" --timestamp=none ${BUNDLE}/Frameworks/libGLESv2.framework
        COMMAND codesign --force --sign "${IOS_CODESIGN_IDENTITY}" --timestamp=none ${IOS_ENTITLEMENTS_FLAG} ${BUNDLE}
        COMMAND ${CMAKE_COMMAND} -E echo "Built ${BUNDLE} for ${IOS_PLATFORM_NAME}"
        # VERBATIM: a real signing identity is a display name like
        # "Apple Development: Max Thomas (LZK7LSMFWU)". Without VERBATIM, CMake
        # escapes the spaces but not the parentheses, and /bin/sh dies with
        # "syntax error near unexpected token `('".
        VERBATIM
        )
endmacro()

macro(plat_link_and_package)
    # CMake defaults MACOSX_BUNDLE to ON for CMAKE_SYSTEM_NAME=iOS, which would put
    # the binary in openjkdf2-ios.app/ with a CMake-generated Info.plist. We lay the
    # bundle out ourselves in postcompile_ios() (it needs the ANGLE frameworks, the
    # templated plist and inside-out signing), so emit a plain executable instead.
    set_target_properties(${BIN_NAME} PROPERTIES MACOSX_BUNDLE FALSE)

    if(TARGET_CAN_JKGM)
        target_link_libraries(sith_engine PRIVATE PNG::PNG ZLIB::ZLIB)
    endif()

    target_link_libraries(sith_engine PRIVATE ${SDL2_COMMON_LIBS})
    target_link_libraries(sith_engine PRIVATE ANGLE::EGL ANGLE::GLESv2)
    target_link_libraries(${BIN_NAME} PRIVATE ANGLE::EGL ANGLE::GLESv2)
    target_link_libraries(sith_engine PRIVATE nlohmann_json::nlohmann_json)

    postcompile_ios()
endmacro()

macro(plat_extra_deps)
    ios_target_add_standard_deps(${BIN_NAME})
endmacro()
