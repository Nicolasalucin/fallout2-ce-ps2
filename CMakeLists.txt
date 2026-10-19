cmake_minimum_required(VERSION 3.13)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(EXECUTABLE_NAME fallout2-ce)

if(APPLE)
    if(IOS)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
    else()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "")
    endif()
endif()

project(${EXECUTABLE_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

option(FALLOUT_VENDORED "Use vendored third-party libraries" ON)

# ---------------------------------------------------------------------------
# PlayStation 2 port (native build against PS2SDK).
#
# Enabled automatically by cmake/toolchain-ps2.cmake (which sets PS2=TRUE) or
# manually with -DPS2_BUILD=ON for inspection. See README.md for the Docker
# build command.
# ---------------------------------------------------------------------------
option(PS2_BUILD "Build for the PlayStation 2 (PS2SDK cross toolchain)" OFF)
option(PS2_DEV_SMOKE_TEST "Enable F6 smoke harness (automated acceptance test)" OFF)
if(PS2_BUILD)
    set(PS2 TRUE)
endif()
if(PS2_DEV_SMOKE_TEST)
    add_compile_definitions(PS2_DEV_SMOKE_TEST)
endif()

if(ANDROID)
    add_library(${EXECUTABLE_NAME} SHARED)
else()
    add_executable(${EXECUTABLE_NAME} WIN32 MACOSX_BUNDLE)
endif()

target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/actions.cc"
    "src/actions.h"
    "src/animation.cc"
    "src/animation.h"
    "src/art.cc"
    "src/art.h"
    "src/audio_file.cc"
    "src/audio_file.h"
    "src/audio.cc"
    "src/audio.h"
    "src/automap.cc"
    "src/automap.h"
    "src/autorun.cc"
    "src/autorun.h"
    "src/cache.cc"
    "src/cache.h"
    "src/character_editor.cc"
    "src/character_editor.h"
    "src/character_selector.cc"
    "src/character_selector.h"
    "src/color.cc"
    "src/color.h"
    "src/combat_ai_defs.h"
    "src/combat_ai.cc"
    "src/combat_ai.h"
    "src/combat_defs.h"
    "src/combat.cc"
    "src/combat.h"
    "src/config.cc"
    "src/config.h"
    "src/credits.cc"
    "src/credits.h"
    "src/critter.cc"
    "src/critter.h"
    "src/cycle.cc"
    "src/cycle.h"
    "src/datafile.cc"
    "src/datafile.h"
    "src/db.cc"
    "src/db.h"
    "src/dbox.cc"
    "src/dbox.h"
    "src/debug.cc"
    "src/debug.h"
    "src/dfile.cc"
    "src/dfile.h"
    "src/dialog.cc"
    "src/dialog.h"
    "src/dictionary.cc"
    "src/dictionary.h"
    "src/dinput.cc"
    "src/dinput.h"
    "src/display_monitor.cc"
    "src/display_monitor.h"
    "src/draw.cc"
    "src/draw.h"
    "src/elevator.cc"
    "src/elevator.h"
    "src/endgame.cc"
    "src/endgame.h"
    "src/export.cc"
    "src/export.h"
    "src/file_find.cc"
    "src/file_find.h"
    "src/file_utils.cc"
    "src/file_utils.h"
    "src/font_manager.cc"
    "src/font_manager.h"
    "src/game_config.cc"
    "src/game_config.h"
    "src/game_dialog.cc"
    "src/game_dialog.h"
    "src/game_memory.cc"
    "src/game_memory.h"
    "src/game_mouse.cc"
    "src/game_mouse.h"
    "src/game_movie.cc"
    "src/game_movie.h"
    "src/game_sound.cc"
    "src/game_sound.h"
    "src/game_vars.h"
    "src/game.cc"
    "src/game.h"
    "src/geometry.cc"
    "src/geometry.h"
    "src/graph_lib.cc"
    "src/graph_lib.h"
    "src/heap.cc"
    "src/heap.h"
    "src/input.cc"
    "src/input.h"
    "src/interface.cc"
    "src/interface.h"
    "src/interpreter_extra.cc"
    "src/interpreter_extra.h"
    "src/interpreter_lib.cc"
    "src/interpreter_lib.h"
    "src/interpreter.cc"
    "src/interpreter.h"
    "src/inventory.cc"
    "src/inventory.h"
    "src/item.cc"
    "src/item.h"
    "src/kb.cc"
    "src/kb.h"
    "src/light.cc"
    "src/light.h"
    "src/lips.cc"
    "src/lips.h"
    "src/loadsave.cc"
    "src/loadsave.h"
    "src/main.cc"
    "src/main.h"
    "src/mainmenu.cc"
    "src/mainmenu.h"
    "src/map_defs.h"
    "src/map.cc"
    "src/map.h"
    "src/memory_defs.h"
    "src/memory_manager.h"
    "src/memory.cc"
    "src/memory.h"
    "src/message.cc"
    "src/message.h"
    "src/mouse_manager.cc"
    "src/mouse_manager.h"
    "src/mouse.cc"
    "src/mouse.h"
    "src/movie_effect.cc"
    "src/movie_effect.h"
    "src/movie_lib.cc"
    "src/movie_lib.h"
    "src/movie.cc"
    "src/movie.h"
    "src/nevs.cc"
    "src/nevs.h"
    "src/obj_types.h"
    "src/object.cc"
    "src/object.h"
    "src/options.cc"
    "src/options.h"
    "src/palette.cc"
    "src/palette.h"
    "src/party_member.cc"
    "src/party_member.h"
    "src/pcx.cc"
    "src/pcx.h"
    "src/perk_defs.h"
    "src/perk.cc"
    "src/perk.h"
    "src/pipboy.cc"
    "src/pipboy.h"
    "src/proto_instance.cc"
    "src/proto_instance.h"
    "src/proto_types.h"
    "src/proto.cc"
    "src/proto.h"
    "src/queue.cc"
    "src/queue.h"
    "src/random.cc"
    "src/random.h"
    "src/reaction.cc"
    "src/reaction.h"
    "src/region.cc"
    "src/region.h"
    "src/scripts.cc"
    "src/scripts.h"
    "src/select_file_list.cc"
    "src/select_file_list.h"
    "src/skill_defs.h"
    "src/skill.cc"
    "src/skill.h"
    "src/skilldex.cc"
    "src/skilldex.h"
    "src/sound_decoder.cc"
    "src/sound_decoder.h"
    "src/sound_effects_cache.cc"
    "src/sound_effects_cache.h"
    "src/sound_effects_list.cc"
    "src/sound_effects_list.h"
    "src/sound.cc"
    "src/sound.h"
    "src/stat_defs.h"
    "src/stat.cc"
    "src/stat.h"
    "src/string_parsers.cc"
    "src/string_parsers.h"
    "src/svga.cc"
    "src/svga.h"
    "src/text_font.cc"
    "src/text_font.h"
    "src/text_object.cc"
    "src/text_object.h"
    "src/tile.cc"
    "src/tile.h"
    "src/trait_defs.h"
    "src/trait.cc"
    "src/trait.h"
    "src/version.cc"
    "src/version.h"
    "src/widget.cc"
    "src/widget.h"
    "src/win32.h"
    "src/window_manager_private.cc"
    "src/window_manager_private.h"
    "src/window_manager.cc"
    "src/window_manager.h"
    "src/window.cc"
    "src/window.h"
    "src/word_wrap.cc"
    "src/word_wrap.h"
    "src/worldmap.cc"
    "src/worldmap.h"
    "src/xfile.cc"
    "src/xfile.h"
)

target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/audio_engine.cc"
    "src/audio_engine.h"
    "src/delay.cc"
    "src/delay.h"
    "src/fps_limiter.cc"
    "src/fps_limiter.h"
    "src/platform_compat.h"
    "src/pointer_registry.cc"
    "src/pointer_registry.h"
    "src/preferences.cc"
    "src/preferences.h"
    "src/settings.cc"
    "src/settings.h"
    "src/sfall_config.cc"
    "src/sfall_config.h"
    "src/sfall_global_vars.cc"
    "src/sfall_global_vars.h"
    "src/sfall_global_scripts.cc"
    "src/sfall_global_scripts.h"
    "src/sfall_ini.cc"
    "src/sfall_ini.h"
    "src/sfall_kb_helpers.cc"
    "src/sfall_kb_helpers.h"
    "src/sfall_lists.cc"
    "src/sfall_lists.h"
    "src/sfall_metarules.cc"
    "src/sfall_metarules.h"
    "src/sfall_opcodes.cc"
    "src/sfall_opcodes.h"
    "src/sfall_arrays.cc"
    "src/sfall_arrays.h"
    "src/touch.cc"
    "src/touch.h"
)

# win32.cc owns the desktop SDL entry point (main -> falloutMain) and the
# storage for gProgramIsActive; on PS2 the platform layer provides its own
# main() (src/platform/ps2/main.cc) so the platform can be brought up before the
# engine starts. platform_compat.cc is likewise replaced on PS2 by
# src/platform/ps2/platform_compat_ps2.cc (device-aware paths).
if(NOT PS2)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "src/win32.cc"
        "src/platform_compat.cc"
    )
endif()

if(PS2)
    # Native PlayStation 2 platform layer (C99) + two C++ shims (entry point
    # and the SDL2-subset implementation). The audio worker streams mixed PCM
    # to the SPU2 through audsrv.
    target_sources(${EXECUTABLE_NAME} PRIVATE
        "src/platform/ps2/main.cc"
        "src/platform/ps2/boot.c"
        "src/platform/ps2/ps2_time.c"
        "src/platform/ps2/ps2_mem.c"
        "src/platform/ps2/ps2_heap.c"
        "src/platform/ps2/ps2_log.c"
        "src/platform/ps2/ps2_perf.c"
        "src/platform/ps2/ps2_mem_guard.c"
        "src/platform/ps2/input_pad.c"
        "src/platform/ps2/video_gs.c"
        "src/platform/ps2/ps2_osk.c"
        "src/platform/ps2/fs.c"
        "src/platform/ps2/ps2_save.c"
        "src/platform/ps2/ps2_storage.c"
        "src/platform/ps2/audio_audsrv.c"
        "src/platform/ps2/platform_compat_ps2.cc"
        "src/platform/ps2/memory_manager_ps2.cc"
        "src/platform/ps2/sdl/sdl_shim.c"
    )

    # The engine sources are flat in src/ while the platform layer lives below
    # src/platform/ps2/, so -Isrc is what lets the platform files include
    # engine headers by name (platform_compat.h, combat.h, ...).
    target_include_directories(${EXECUTABLE_NAME} PRIVATE "src")

    # audsrv.irx (ps2sdk, BSD/AFL-2.0) is embedded into the ELF; on a real
    # PS2 that embedded copy is the module that actually gets used, so no
    # loose files need to ship next to the game data.
    file(READ "${PS2SDK}/iop/irx/audsrv.irx" _ps2_audsrv_irx_hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," _ps2_audsrv_irx_bytes "${_ps2_audsrv_irx_hex}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/audsrv_irx_data.c"
        "/* Generated from ps2sdk audsrv.irx - do not edit. */\n"
        "const unsigned char g_audsrv_irx[] = { ${_ps2_audsrv_irx_bytes} 0x00 };\n"
        "const unsigned int g_audsrv_irx_size = sizeof(g_audsrv_irx) - 1;\n")
    target_sources(${EXECUTABLE_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/audsrv_irx_data.c")

    # IOP drivers for USB mass storage are embedded the same way, so a raw-ELF
    # boot (uLaunchELF "Launch Elf", no preloaded IOP stack) can read game data
    # from mass: without shipping loose .irx files. The tiered loading in
    # ps2_storage.c only touches them when the device is not already provided
    # by the loader (OPL/uLaunchELF/ps2link/PCSX2).
    set(_ps2_storage_irx_modules usbd usbhdfsd bdm usbmass_bd bdmfs_fatfs sio2man mx4sio_bd)
    set(_ps2_storage_irx_sources "")
    foreach(_ps2_irx ${_ps2_storage_irx_modules})
        file(READ "${PS2SDK}/iop/irx/${_ps2_irx}.irx" _ps2_irx_hex HEX)
        string(REGEX REPLACE "(..)" "0x\\1," _ps2_irx_bytes "${_ps2_irx_hex}")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ps2_${_ps2_irx}_irx_data.c"
            "/* Generated from ps2sdk ${_ps2_irx}.irx - do not edit. */\n"
            "const unsigned char g_ps2_${_ps2_irx}_irx[] = { ${_ps2_irx_bytes} 0x00 };\n"
            "const unsigned int g_ps2_${_ps2_irx}_irx_size = sizeof(g_ps2_${_ps2_irx}_irx) - 1;\n")
        list(APPEND _ps2_storage_irx_sources "${CMAKE_CURRENT_BINARY_DIR}/ps2_${_ps2_irx}_irx_data.c")
    endforeach()
    target_sources(${EXECUTABLE_NAME} PRIVATE ${_ps2_storage_irx_sources})

    # The same module is also dropped next to the ELF. Under PCSX2 the
    # EE->IOP DMA route of SifExecModuleBuffer is broken by the IOP HLE (the
    # module loads on paper but never runs), and SifLoadModule of this file
    # is the only working route there; PCSX2 resolves "host:" to the ELF's
    # directory. See src/platform/ps2/audio_audsrv.c.
    add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PS2SDK}/iop/irx/audsrv.irx"
                "${CMAKE_CURRENT_BINARY_DIR}/audsrv.irx"
        COMMENT "Copying audsrv.irx next to the game ELF (PCSX2 host: root)"
    )

    # Development convenience: PCSX2 resolves "host:" to the ELF's directory,
    # so the game data has to be reachable from the build directory. When the
    # repo has a local_assets folder (the standard place for a local install;
    # it is never committed), link it next to the ELF so emulator runs need no
    # manual setup. Copying the data is not an option: it is ~500 MiB.
    if(CMAKE_HOST_UNIX AND EXISTS "${CMAKE_SOURCE_DIR}/local_assets")
        add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "../local_assets" "${CMAKE_CURRENT_BINARY_DIR}/local_assets"
            COMMENT "Linking local_assets next to the game ELF (PCSX2 host: root)"
        )
    endif()

    # The SDL2-subset shim (src/platform/ps2/sdl/SDL.h) must win over any
    # other SDL.h on the include path.
    target_include_directories(${EXECUTABLE_NAME} BEFORE PRIVATE "src/platform/ps2/sdl")
    target_include_directories(${EXECUTABLE_NAME} PRIVATE "src/platform/ps2")

    target_compile_options(${EXECUTABLE_NAME} PRIVATE
        -Wcast-align=strict
        -fno-strict-aliasing
    )

    # Requested optimization profile for the EE (R5900 has no hardware FP64).
    # NOTE: -fsingle-precision-constant is applied only to the PS2 platform
    # layer (see below), not to the upstream engine: turning every floating
    # literal into `float` breaks upstream code (e.g. std::clamp<double>
    # overload resolution in options.cc).
    target_compile_options(${EXECUTABLE_NAME} PRIVATE
        $<$<CONFIG:Release>:-ffast-math>
    )

    # PS2 platform layer: single-precision floating constants are fine here.
    # -Wno-cast-align: this layer deliberately casts between byte buffers and
    # typed DMA/audio buffers (all accesses are statically aligned in
    # practice); the strict cast-align audit stays enabled for the engine.
    set_source_files_properties(
        src/platform/ps2/boot.c
        src/platform/ps2/ps2_time.c
        src/platform/ps2/ps2_mem.c
        src/platform/ps2/ps2_log.c
        src/platform/ps2/ps2_perf.c
        src/platform/ps2/input_pad.c
        src/platform/ps2/video_gs.c
        src/platform/ps2/ps2_osk.c
        src/platform/ps2/fs.c
        src/platform/ps2/audio_audsrv.c
        src/platform/ps2/platform_compat_ps2.cc
        src/platform/ps2/sdl/sdl_shim.c
        PROPERTIES COMPILE_OPTIONS "-fsingle-precision-constant;-Wno-cast-align"
    )

    # Platform libraries: gsKit (+ DMA/kit utilities), GS graph primitives,
    # libpad for DualShock 2 input, libaudsrv for SPU2 streaming. zlib comes
    # from ps2sdk's ports collection: the engine already inflates DAT entries
    # with it (src/dfile.cc), and building the vendored copy for the EE would
    # only duplicate what the SDK ships.
    target_link_libraries(${EXECUTABLE_NAME}
        gskit
        dmakit
        graph
        pad
        audsrv
        z
        m
    )

    # Canonical PS2 artifact name: FALLOUT2.ELF (the boot descriptor and the
    # release bundle refer to this name).
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
        OUTPUT_NAME "FALLOUT2"
        SUFFIX ".ELF"
    )
endif()

if(IOS)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "src/platform/ios/paths.h"
        "src/platform/ios/paths.mm"
    )
endif()

if(WIN32)
    target_compile_definitions(${EXECUTABLE_NAME} PUBLIC
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME}
        winmm
    )
endif()

if(WIN32)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "os/windows/fallout2-ce.ico"
        "os/windows/fallout2-ce.rc"
    )
endif()

if(APPLE)
    if(IOS)
        set(RESOURCES
            "os/ios/AppIcon.xcassets"
            "os/ios/LaunchScreen.storyboard"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/ios/Info.plist"
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout2-ce"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        )

        set(MACOSX_BUNDLE_BUNDLE_NAME "${EXECUTABLE_NAME}")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout 2")
    else()
        set(RESOURCES
            "os/macos/fallout2-ce.icns"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            OUTPUT_NAME "Fallout II Community Edition"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/macos/Info.plist"
            XCODE_ATTRIBUTE_EXECUTABLE_NAME "${EXECUTABLE_NAME}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout2-ce"
        )

        set(MACOSX_BUNDLE_ICON_FILE "fallout2-ce.icns")
        set(MACOSX_BUNDLE_BUNDLE_NAME "Fallout II: Community Edition")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout II")
    endif()

    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.alexbatalov.fallout2-ce")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.3.0")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "1.3.0")
endif()

add_subdirectory("third_party/fpattern")
target_link_libraries(${EXECUTABLE_NAME} fpattern::fpattern)

# Desktop/mobile SDL2. On PS2 the SDL2 subset in src/platform/ps2/sdl/ is
# compiled straight into the ELF instead, both the header and the
# implementation (see the PS2 block above).
if(NOT PS2)
    if(FALLOUT_VENDORED)
        add_subdirectory("third_party/sdl2")
    else()
        find_package(SDL2)
    endif()

    target_link_libraries(${EXECUTABLE_NAME} ${SDL2_LIBRARIES})
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

# zlib: the DAT reader (src/dfile.cc) inflates with it, so every build needs it.
# The PS2 build links ps2sdk's ported zlib from the toolchain instead (EE
# object code, headers already on the include path); the vendored copy is a
# host library and cannot be used for the R5900.
if(NOT PS2)
    if(FALLOUT_VENDORED)
        add_subdirectory("third_party/zlib")
    else()
        find_package(ZLIB)
    endif()

    target_link_libraries(${EXECUTABLE_NAME} ${ZLIB_LIBRARIES})
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${ZLIB_INCLUDE_DIRS})
endif()

if(APPLE)
    if(IOS)
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION "Payload")

        set(CPACK_GENERATOR "ZIP")
        set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
        set(CPACK_PACKAGE_FILE_NAME "fallout2-ce")
        set(CPACK_ARCHIVE_FILE_EXTENSION "ipa")
    else()
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION .)

        set(CPACK_GENERATOR "DragNDrop")
        set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK ON)
        set(CPACK_PACKAGE_FILE_NAME "Fallout II Community Edition")
    endif()

    include(CPack)
endif()
