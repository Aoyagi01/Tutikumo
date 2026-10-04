set(TUTIKUMO_PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# The AOT corpus is derived from the user's executable and is not committed.
# Until scripts/generate.sh has produced it, link an empty registry instead.
file(GLOB TUTIKUMO_GENERATED CONFIGURE_DEPENDS "${TUTIKUMO_PROFILE_DIR}/generated/*.cpp")
if(TUTIKUMO_GENERATED)
    list(LENGTH TUTIKUMO_GENERATED TUTIKUMO_GENERATED_COUNT)
    message(STATUS "tutikumo: ${TUTIKUMO_GENERATED_COUNT} generated AOT units")
    if(MSVC)
        if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
            set(TUTIKUMO_GENERATED_OPTIONS "/Od;/bigobj")
        else()
            set(TUTIKUMO_GENERATED_OPTIONS "/O${PSPRECOMP_GENERATED_OPT_LEVEL};/Ob0;/bigobj")
        endif()
    else()
        set(TUTIKUMO_GENERATED_OPTIONS "-O${PSPRECOMP_GENERATED_OPT_LEVEL};-g0")
    endif()
    set_source_files_properties(${TUTIKUMO_GENERATED} PROPERTIES COMPILE_OPTIONS "${TUTIKUMO_GENERATED_OPTIONS}")

    # The analog camera wraps the game's camera rotation helper, and main.cpp
    # has to hand it the generated unit that helper was compiled into. Which
    # unit that is depends on how the generator partitions the code, so read it
    # from the corpus instead of naming it: a new partition then finds its own
    # unit, and a corpus without the helper stops the build here rather than
    # letting the camera call the wrong code in game.
    set(TUTIKUMO_CAMERA_HELPER_UNIT "")
    foreach(unit IN LISTS TUTIKUMO_GENERATED)
        # MHP2G: the camera update (host/camera/game_camera.cpp, kMhp2gCameraUpdate).
        file(STRINGS "${unit}" hit REGEX "register_function\\(0x08886B54u, &recomp_unit_[0-9]+," LIMIT_COUNT 1)
        if(hit)
            string(REGEX MATCH "recomp_unit_[0-9]+" TUTIKUMO_CAMERA_HELPER_UNIT "${hit}")
            break()
        endif()
    endforeach()
    # MHP2G has no such helper at that address: the MHP3rd HD camera driver
    # stays off until MHP2G's own is written.
    # MHP2G: the hunter state check the camera update calls right before it
    # reads its preset (kMhp2gStateCheck), which the vertical camera wraps.
    set(MHP2G_STATE_CHECK_UNIT "")
    foreach(unit IN LISTS TUTIKUMO_GENERATED)
        file(STRINGS "${unit}" hit REGEX "register_function\\(0x08865CB0u, &recomp_unit_[0-9]+," LIMIT_COUNT 1)
        if(hit)
            string(REGEX MATCH "recomp_unit_[0-9]+" MHP2G_STATE_CHECK_UNIT "${hit}")
            break()
        endif()
    endforeach()
    if(TUTIKUMO_CAMERA_HELPER_UNIT)
        message(STATUS "mhp2g: camera rotation helper in ${TUTIKUMO_CAMERA_HELPER_UNIT}, state check in ${MHP2G_STATE_CHECK_UNIT}")
    else()
        message(STATUS "mhp2g: no MHP3rd camera rotation helper; analog camera driver off")
    endif()

    # Hiding the HUD (host/gpu/game_hud.cpp) wraps the game's 2D functions the
    # same way, so their units are read from the corpus too, in one pass.
    set(TUTIKUMO_HUD_FUNCTIONS
        08876874 088EA9B0 088EB07C 088EBF0C 088EC060 088EC0F8 088EC1F0 088EC2EC 088EC3E0
        088EC63C 088ECB74 088ECC38 088ECCE4 088ECD9C 088ECE58)
    list(JOIN TUTIKUMO_HUD_FUNCTIONS "|" hud_pattern)
    set(hud_lines "")
    foreach(unit IN LISTS TUTIKUMO_GENERATED)
        file(STRINGS "${unit}" hits REGEX "register_function\\(0x(${hud_pattern})u, &recomp_unit_[0-9]+,")
        foreach(hit IN LISTS hits)
            string(REGEX MATCH "0x[0-9A-F]+u, &recomp_unit_[0-9]+" pair "${hit}")
            string(REPLACE ", &" ", " pair "${pair}")
            string(APPEND hud_lines "TUTIKUMO_HUD_FUNCTION(${pair})\n")
        endforeach()
    endforeach()
    set(TUTIKUMO_HUD_INC "${CMAKE_CURRENT_BINARY_DIR}/generated_hooks/hud_functions.inc")
    file(CONFIGURE OUTPUT "${TUTIKUMO_HUD_INC}" CONTENT "${hud_lines}" @ONLY)

    # Layered armor (host/game/layered_armor_hook.cpp) stands in for the game's
    # lookup of an armor part's model and wraps the hunter's load step, so
    # their units are read from the corpus too.
    set(TUTIKUMO_LAYERED_FUNCTIONS 08869778 088A5564)
    list(JOIN TUTIKUMO_LAYERED_FUNCTIONS "|" layered_pattern)
    set(layered_lines "")
    foreach(unit IN LISTS TUTIKUMO_GENERATED)
        file(STRINGS "${unit}" hits REGEX "register_function\\(0x(${layered_pattern})u, &recomp_unit_[0-9]+,")
        foreach(hit IN LISTS hits)
            string(REGEX MATCH "0x[0-9A-F]+u, &recomp_unit_[0-9]+" pair "${hit}")
            string(REPLACE ", &" ", " pair "${pair}")
            string(APPEND layered_lines "TUTIKUMO_LAYERED_FUNCTION(${pair})\n")
        endforeach()
    endforeach()
    set(TUTIKUMO_LAYERED_INC "${CMAKE_CURRENT_BINARY_DIR}/generated_hooks/layered_armor_functions.inc")
    file(CONFIGURE OUTPUT "${TUTIKUMO_LAYERED_INC}" CONTENT "${layered_lines}" @ONLY)
    # A regenerated corpus with the same file names must be read again.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TUTIKUMO_GENERATED})
else()
    message(STATUS "tutikumo: no generated AOT units; run profiles/tutikumo/scripts/generate.sh")
    set(TUTIKUMO_GENERATED host/generated_stub.cpp)
endif()

# Renderer: SDL3 for the window and Vulkan for drawing. Both are optional so the
# profile still builds headless on a machine without them.
option(TUTIKUMO_RENDERER "Build the Vulkan renderer" ON)
set(TUTIKUMO_RENDERER_SOURCES)
if(TUTIKUMO_RENDERER)
    find_package(SDL3 QUIET)
    find_package(Vulkan QUIET)
    find_program(TUTIKUMO_GLSLANG NAMES glslangValidator glslang)
    if(SDL3_FOUND AND Vulkan_FOUND AND TUTIKUMO_GLSLANG)
        set(TUTIKUMO_SHADER_INC "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders/ge_shaders.inc")
        set(TUTIKUMO_SHADERS
            "kGeVertexShader=${TUTIKUMO_PROFILE_DIR}/host/gpu/shaders/ge.vert"
            "kGeRawVertexShader=GE_RAW_VERTICES@${TUTIKUMO_PROFILE_DIR}/host/gpu/shaders/ge.vert"
            "kGeCheckVertexShader=GE_RAW_VERTICES,GE_CHECK_DECODE@${TUTIKUMO_PROFILE_DIR}/host/gpu/shaders/ge.vert"
            "kGeFragmentShader=${TUTIKUMO_PROFILE_DIR}/host/gpu/shaders/ge.frag"
            "kGeFragmentShaderPlain=GE_NO_SPECIALIZATION@${TUTIKUMO_PROFILE_DIR}/host/gpu/shaders/ge.frag")
        if(ANDROID)
            # Pre-rotation of the finished frame for a display turned sideways.
            list(APPEND TUTIKUMO_SHADERS
                "kRotateVertexShader=${TUTIKUMO_PROFILE_DIR}/host/gpu/shaders/rotate.vert"
                "kRotateFragmentShader=${TUTIKUMO_PROFILE_DIR}/host/gpu/shaders/rotate.frag")
        endif()
        set(TUTIKUMO_SHADER_SOURCES ${TUTIKUMO_SHADERS})
        list(TRANSFORM TUTIKUMO_SHADER_SOURCES REPLACE "^[A-Za-z]+=([A-Z_,]+@)?" "")
        list(REMOVE_DUPLICATES TUTIKUMO_SHADER_SOURCES)
        add_custom_command(
            OUTPUT "${TUTIKUMO_SHADER_INC}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
            COMMAND python3 "${TUTIKUMO_PROFILE_DIR}/tools/embed_shaders.py" "${TUTIKUMO_GLSLANG}" "${TUTIKUMO_SHADER_INC}"
                    ${TUTIKUMO_SHADERS}
            DEPENDS ${TUTIKUMO_SHADER_SOURCES}
                    "${TUTIKUMO_PROFILE_DIR}/tools/embed_shaders.py"
            COMMENT "Compiling GE shaders to SPIR-V")
        add_custom_target(tutikumo_shaders DEPENDS "${TUTIKUMO_SHADER_INC}")
        # Dear ImGui (third_party/imgui, unmodified) draws the port's own
        # interface over the game.
        set(TUTIKUMO_RENDERER_SOURCES
            host/gpu/descriptor_pools.cpp
            host/gpu/device_report.cpp
            host/gpu/frame_interpolation.cpp
            host/gpu/frame_pacing.cpp
            host/gpu/replacement_textures.cpp
            host/gpu/mmpx.cpp
            host/gpu/ui_textures.cpp
            host/gpu/vulkan_renderer.cpp
            host/input/gamepad_devices.cpp
            host/ui/bindings_editor.cpp
            host/ui/controllers_screen.cpp
            host/ui/file_browser.cpp
            host/ui/font_menu.cpp
            host/ui/i18n.cpp
            host/ui/input_script.cpp
            host/ui/layer.cpp
            host/ui/menu.cpp
            host/ui/layered_armor_screen.cpp
            host/ui/mods_screen.cpp
            host/ui/save_screen.cpp
            host/ui/setup_screens.cpp
            host/ui/text_input.cpp
            host/ui/touch_editor.cpp
            host/ui/touch_overlay.cpp
            host/ui/texture_pack_screen.cpp
            host/ui/widgets.cpp
            third_party/imgui/imgui.cpp
            third_party/imgui/imgui_draw.cpp
            third_party/imgui/imgui_tables.cpp
            third_party/imgui/imgui_widgets.cpp
            third_party/imgui/backends/imgui_impl_sdl3.cpp
            third_party/imgui/backends/imgui_impl_vulkan.cpp)
        message(STATUS "tutikumo: Vulkan renderer enabled")
    else()
        message(STATUS "tutikumo: renderer disabled (SDL3, Vulkan or glslang not found)")
        set(TUTIKUMO_RENDERER OFF)
    endif()
endif()

# The build the in-game menu shows: `git describe` of the checkout, refreshed
# on every build but rewritten only when it changes, so only the menu
# recompiles after a commit.
set(TUTIKUMO_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated_version/tutikumo_version.hpp")
add_custom_target(tutikumo_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${TUTIKUMO_VERSION_HEADER}
            -P "${TUTIKUMO_PROFILE_DIR}/tools/write_version.cmake"
    BYPRODUCTS "${TUTIKUMO_VERSION_HEADER}"
    COMMENT "Checking the build version")

# FFmpeg decodes the game's music and its movies; by default it is built with
# the project (see cmake/FFmpeg.cmake).
include("${TUTIKUMO_PROFILE_DIR}/cmake/FFmpeg.cmake")

# GE parsing, texture decoding and the audio mixer are backend independent and
# always built; only the device the sink opens depends on SDL.
set(TUTIKUMO_HOST_SOURCES
    host/app_paths.cpp
    host/audio/atrac_decoder.cpp
    host/movie/avc_decoder.cpp
    host/movie/psmf_demuxer.cpp
    host/audio/audio_sink.cpp
    host/audio/sas_core.cpp
    host/camera_probe.cpp
    host/camera/camera_input.cpp
    host/camera/game_aspect.cpp
    host/camera/game_camera.cpp
    host/camera/lock_on.cpp
    host/camera/free_camera.cpp
    host/gpu/game_hud.cpp
    host/gpu/ge_state.cpp
    host/gpu/texture_decode.cpp
    host/input/bindings.cpp
    host/input/chords.cpp
    host/input/presets.cpp
    host/input/gamepad_mapping.cpp
    host/input/touch_action.cpp
    host/input/touch_controls.cpp
    host/gpu/texture_pack.cpp
    host/gpu/texture_pack_import.cpp
    host/gpu/screenshot.cpp
    host/perf/frame_stats.cpp
    host/perf/perf_overlay.cpp
    host/platform/utf8_path.cpp
    host/overlays.cpp
    host/tutikumo_profile.cpp
    host/kernel/kernel.cpp
    host/kernel/load_trace.cpp
    host/kernel/fast_loading.cpp
    host/kernel/fast_forward.cpp
    host/kernel/fast_forward_switch.cpp
    host/kernel/load_detector.cpp
    host/kernel/iso_image.cpp
    host/hle/hle_common.cpp
    host/hle/hle_threadman.cpp
    host/hle/hle_sysmem.cpp
    host/hle/hle_io.cpp
    host/hle/hle_system.cpp
    host/hle/hle_media.cpp
    host/hle/hle_atrac.cpp
    host/hle/hle_mpeg.cpp
    host/fonts/game_font.cpp
    host/hle/hle_font.cpp
    host/hle/hle_utility.cpp
    host/hle/hle_savedata.cpp
    host/hle/hle_adhoc.cpp
    host/adhoc/client.cpp
    host/adhoc/discovery.cpp
    host/adhoc/host.cpp
    host/adhoc/server.cpp
    host/save_data/aes128.cpp
    host/save_data/param_sfo.cpp
    host/save_data/savedata_crypto.cpp
    host/save_data/savedata_store.cpp
    host/save_data/save_transfer.cpp
    host/install/executable_preparation.cpp
    host/install/front_end.cpp
    host/install/installer.cpp
    host/install/installer_dialogs.cpp
    host/install/user_data.cpp
    host/settings/settings.cpp
    host/mods/file_overlay.cpp
    host/mods/mod_import.cpp
    host/mods/mod_ini.cpp
    host/mods/mod_library.cpp
    host/mods/mod_session.cpp
    host/mods/tutikumo_data_bin.cpp
    host/mods/tutikumo_mod_format.cpp
    host/mods/tutikumo_mods.cpp
    host/game/guest_ram.cpp
    host/game/game_data.cpp
    host/game/equipment_models.cpp
    host/game/layered_armor.cpp
    host/game/layered_armor_hook.cpp
    third_party/tiny_aes/aes.c
)
# tiny-AES-c is C; the project only enables C++, which compiles it unchanged.
set_source_files_properties(third_party/tiny_aes/aes.c PROPERTIES LANGUAGE CXX)

# Basis Universal's transcoder (third_party/basisu, unmodified) reads the KTX2
# images of HD texture packs; its single-file Zstandard decoder is C.
enable_language(C)
add_library(mhp2g_basisu STATIC
    third_party/basisu/transcoder/basisu_transcoder.cpp
    third_party/basisu/zstd/zstddeclib.c)
target_compile_definitions(mhp2g_basisu PUBLIC BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1)
target_include_directories(mhp2g_basisu PUBLIC "${TUTIKUMO_PROFILE_DIR}/third_party")
if(MSVC)
    target_compile_options(mhp2g_basisu PRIVATE /W0 /bigobj)
else()
    target_compile_options(mhp2g_basisu PRIVATE -w)
endif()

# The recompiled game code gets a target of its own. It needs only the
# psprecomp headers, so definitions and include paths the host adds to
# Tutikumo (a renderer switch, a new library's headers) do not change its
# compile commands and rebuild all of it. Its objects still link straight into
# the executable, which exports their symbols to the overlay libraries.
add_library(tutikumo_generated OBJECT ${TUTIKUMO_GENERATED})
target_include_directories(tutikumo_generated PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${TUTIKUMO_PROFILE_DIR}/generated")
set_target_properties(tutikumo_generated PROPERTIES JOB_POOL_COMPILE psprecomp_generated)

# An Android app is a shared library, libmain.so, that SDL's Java activity
# loads and calls; the command-line executable still builds for Android without
# this and runs from adb shell.
option(TUTIKUMO_ANDROID_APP "Build the Android app's libmain.so instead of an executable" OFF)
if(TUTIKUMO_ANDROID_APP AND NOT (ANDROID AND TUTIKUMO_RENDERER))
    message(FATAL_ERROR "TUTIKUMO_ANDROID_APP needs the Android NDK toolchain and the renderer (SDL3 and Vulkan)")
endif()
set(TUTIKUMO_PROGRAM_SOURCES
    host/main.cpp
    ${TUTIKUMO_HOST_SOURCES}
    ${TUTIKUMO_RENDERER_SOURCES}
    $<TARGET_OBJECTS:tutikumo_generated>)
if(TUTIKUMO_ANDROID_APP)
    add_library(Tutikumo SHARED ${TUTIKUMO_PROGRAM_SOURCES} host/platform/android_app.cpp
        host/platform/android_jni.cpp host/platform/android_documents.cpp host/platform/android_performance.cpp
        host/platform/android_fatal.cpp)
    target_compile_definitions(Tutikumo PRIVATE TUTIKUMO_ANDROID_APP=1)
    set_target_properties(Tutikumo PROPERTIES
        OUTPUT_NAME main
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
else()
    add_executable(Tutikumo ${TUTIKUMO_PROGRAM_SOURCES})
endif()
if(WIN32)
    set(TUTIKUMO_WINDOWS_ICON "${TUTIKUMO_PROFILE_DIR}/packaging/windows/emblem.ico")
    configure_file(packaging/windows/Tutikumo.rc.in Tutikumo.rc @ONLY)
    target_sources(Tutikumo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Tutikumo.rc")
endif()
if(TUTIKUMO_RENDERER)
    add_dependencies(Tutikumo tutikumo_shaders tutikumo_version)
    # The sink opens its own SDL audio subsystem, so it only needs SDL to exist.
    target_compile_definitions(Tutikumo PRIVATE TUTIKUMO_HAS_RENDERER=1 TUTIKUMO_HAS_SDL_AUDIO=1 TUTIKUMO_HAS_SDL=1)
    target_include_directories(Tutikumo PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders"
        "${CMAKE_CURRENT_BINARY_DIR}/generated_version"
        "${TUTIKUMO_PROFILE_DIR}/third_party/imgui")
    target_link_libraries(Tutikumo PRIVATE SDL3::SDL3 Vulkan::Vulkan)
endif()
# The host only: the generated code never sees FFmpeg.
tutikumo_use_ffmpeg(Tutikumo)
target_include_directories(Tutikumo PRIVATE
    "${TUTIKUMO_PROFILE_DIR}/host"
    "${TUTIKUMO_PROFILE_DIR}/third_party"
    "${TUTIKUMO_PROFILE_DIR}/generated")
target_link_libraries(Tutikumo PRIVATE psprecomp_core mhp2g_basisu ${CMAKE_DL_LIBS})
if(WIN32)
    # The ad hoc client's and server's sockets, and the list of interfaces;
    # CommandLineToArgvW for the arguments as UTF-8.
    target_link_libraries(Tutikumo PRIVATE ws2_32 iphlpapi shell32)
endif()
find_package(Threads REQUIRED)
target_link_libraries(Tutikumo PRIVATE Threads::Threads)
# The overlay libraries resolve psprecomp and runtime symbols against the
# executable that loads them, so it has to export them.
set_target_properties(Tutikumo PROPERTIES
    ENABLE_EXPORTS TRUE
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
# Only main.cpp needs the camera helper's unit; keeping it off the target keeps
# it off every other unit's command line.
if(TUTIKUMO_CAMERA_HELPER_UNIT)
    set_property(SOURCE host/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        TUTIKUMO_CAMERA_HELPER_UNIT=${TUTIKUMO_CAMERA_HELPER_UNIT})
endif()
if(MHP2G_STATE_CHECK_UNIT)
    set_property(SOURCE host/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        MHP2G_STATE_CHECK_UNIT=${MHP2G_STATE_CHECK_UNIT})
endif()
if(TUTIKUMO_HUD_INC)
    set_property(SOURCE host/gpu/game_hud.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        "TUTIKUMO_HUD_FUNCTIONS_INC=\"${TUTIKUMO_HUD_INC}\"")
endif()
if(TUTIKUMO_LAYERED_INC)
    set_property(SOURCE host/game/layered_armor_hook.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        "TUTIKUMO_LAYERED_FUNCTIONS_INC=\"${TUTIKUMO_LAYERED_INC}\"")
endif()
# The NID table is compiled in, so the program reads nothing from the checkout
# it was built in.
set(TUTIKUMO_NIDS_CSV "${PROJECT_SOURCE_DIR}/configs/nids.csv")
set(TUTIKUMO_NIDS_INC "${CMAKE_CURRENT_BINARY_DIR}/generated_nids/nid_table.inc")
add_custom_command(
    OUTPUT "${TUTIKUMO_NIDS_INC}"
    COMMAND ${CMAKE_COMMAND} -DINPUT=${TUTIKUMO_NIDS_CSV} -DOUTPUT=${TUTIKUMO_NIDS_INC}
            -P "${TUTIKUMO_PROFILE_DIR}/tools/embed_nids.cmake"
    DEPENDS "${TUTIKUMO_NIDS_CSV}" "${TUTIKUMO_PROFILE_DIR}/tools/embed_nids.cmake"
    COMMENT "Embedding the NID table")
target_sources(Tutikumo PRIVATE "${TUTIKUMO_NIDS_INC}")
target_include_directories(Tutikumo PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_nids")

# A release build runs from wherever it is unpacked: it has no fallback to
# this checkout's game directory, and on Linux it finds the libraries it ships
# in lib/ next to the executable and nothing else.
option(TUTIKUMO_RELEASE "Build for distribution: no checkout paths, libraries from lib/ next to the executable" OFF)
if(NOT TUTIKUMO_RELEASE)
    # The checkout's profiles/tutikumo/game, a developer fallback after the
    # per-user data directory. Only main.cpp reads it; on the target it would be
    # on every generated unit's command line, so moving the checkout would
    # rebuild all of them and no other checkout could reuse their cache entries.
    set_property(SOURCE host/main.cpp APPEND PROPERTY COMPILE_DEFINITIONS
        TUTIKUMO_DEFAULT_GAME_DIR="${TUTIKUMO_PROFILE_DIR}/game")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(Tutikumo PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN/lib")
    # The C++ runtime is linked in statically, so a build made against an old
    # glibc runs wherever that glibc does, and its symbols stay private: the
    # executable exports its own symbols to the overlay libraries, and a
    # second, exported libstdc++ would clash with the system's one that the
    # graphics driver loads.
    set(TUTIKUMO_RELEASE_LINK_OPTIONS -static-libstdc++ "LINKER:--exclude-libs,libstdc++.a:libsupc++.a")
    target_link_options(Tutikumo PRIVATE ${TUTIKUMO_RELEASE_LINK_OPTIONS})
endif()
set_target_properties(Tutikumo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    JOB_POOL_COMPILE psprecomp_generated)

# Developer tools (host/debug): the menu's Debug page, with cheats for testing,
# and the command file that drives it from a script. Never in a release build:
# TUTIKUMO_RELEASE compiles all of it out whatever this option says. A developer
# build still shows it only when TUTIKUMO_DEBUG_MENU=1 is set at run time.
option(TUTIKUMO_DEBUG_MENU "Compile the developer tools (Debug page) into non-release builds" ON)
if(TUTIKUMO_DEBUG_MENU AND NOT TUTIKUMO_RELEASE)
    target_sources(Tutikumo PRIVATE
        host/debug/debug_console.cpp
        host/debug/debug_tools.cpp
        host/debug/game_state.cpp
        host/debug/quest_start.cpp)
    if(TUTIKUMO_RENDERER)
        target_sources(Tutikumo PRIVATE host/ui/debug_screen.cpp)
    endif()
    target_compile_definitions(Tutikumo PRIVATE TUTIKUMO_DEBUG_MENU=1)
    message(STATUS "tutikumo: developer tools compiled in (shown with TUTIKUMO_DEBUG_MENU=1)")

    # The game's structures as the tools read and write them, on a buffer.
    add_executable(tutikumo_debug_tools_tests
        tests/debug_tools_tests.cpp
        host/debug/game_state.cpp
        host/debug/quest_start.cpp
        host/game/game_data.cpp)
    target_include_directories(tutikumo_debug_tools_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
    set_target_properties(tutikumo_debug_tools_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME tutikumo_debug_tools_tests COMMAND tutikumo_debug_tools_tests)
    endif()
else()
    message(STATUS "tutikumo: developer tools not compiled in")
endif()

# Save-data checks that need no game data: crypto test vectors and round trips.
add_executable(tutikumo_savedata_tests
    tests/savedata_tests.cpp
    host/save_data/aes128.cpp
    host/save_data/param_sfo.cpp
    host/save_data/savedata_crypto.cpp
    host/save_data/savedata_store.cpp
    host/save_data/save_transfer.cpp
    host/platform/utf8_path.cpp
    third_party/tiny_aes/aes.c)
target_include_directories(tutikumo_savedata_tests PRIVATE
    "${TUTIKUMO_PROFILE_DIR}/host"
    "${TUTIKUMO_PROFILE_DIR}/third_party")
set_target_properties(tutikumo_savedata_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_savedata_tests COMMAND tutikumo_savedata_tests)
endif()

# Camera dispatch, ownership and integration checks without game data.
add_executable(tutikumo_camera_tests
    tests/analog_camera_tests.cpp
    host/camera/camera_input.cpp
    host/camera/game_camera.cpp
    host/camera/lock_on.cpp
    host/input/bindings.cpp
    host/input/chords.cpp
    host/input/presets.cpp
    host/input/touch_action.cpp
    host/input/touch_controls.cpp
    host/platform/utf8_path.cpp)
target_include_directories(tutikumo_camera_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
target_link_libraries(tutikumo_camera_tests PRIVATE psprecomp_core)
set_target_properties(tutikumo_camera_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_camera_tests COMMAND tutikumo_camera_tests)
endif()

# Lock-on: which monster a tap picks, and how it turns the game's camera.
add_executable(tutikumo_lock_on_tests
    tests/lock_on_tests.cpp
    host/camera/camera_input.cpp
    host/camera/game_camera.cpp
    host/camera/lock_on.cpp
    host/input/bindings.cpp
    host/input/chords.cpp
    host/input/presets.cpp
    host/input/touch_action.cpp
    host/input/touch_controls.cpp
    host/platform/utf8_path.cpp)
target_include_directories(tutikumo_lock_on_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
target_link_libraries(tutikumo_lock_on_tests PRIVATE psprecomp_core)
set_target_properties(tutikumo_lock_on_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_lock_on_tests COMMAND tutikumo_lock_on_tests)
endif()

# The free camera's geometry, and that it hooks nothing while off.
add_executable(tutikumo_free_camera_tests
    tests/free_camera_tests.cpp
    host/camera/free_camera.cpp
    host/settings/settings.cpp
    host/install/user_data.cpp
    host/input/bindings.cpp
    host/input/chords.cpp
    host/input/presets.cpp
    host/input/touch_action.cpp
    host/input/touch_controls.cpp
    host/app_paths.cpp
    host/platform/utf8_path.cpp)
target_include_directories(tutikumo_free_camera_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
target_link_libraries(tutikumo_free_camera_tests PRIVATE psprecomp_core)
set_target_properties(tutikumo_free_camera_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_free_camera_tests COMMAND tutikumo_free_camera_tests)
endif()

# The game's view shape: what is written to guest memory, and what is left.
add_executable(tutikumo_aspect_tests
    tests/game_aspect_tests.cpp
    host/camera/game_aspect.cpp)
target_include_directories(tutikumo_aspect_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
target_link_libraries(tutikumo_aspect_tests PRIVATE psprecomp_core)
set_target_properties(tutikumo_aspect_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_aspect_tests COMMAND tutikumo_aspect_tests)
endif()

# Each platform's settings defaults.
add_executable(tutikumo_settings_tests
    tests/settings_defaults_tests.cpp
    host/settings/settings.cpp
    host/install/user_data.cpp
    host/app_paths.cpp
    host/input/bindings.cpp
    host/input/chords.cpp
    host/input/presets.cpp
    host/input/touch_action.cpp
    host/input/touch_controls.cpp
    host/platform/utf8_path.cpp)
target_include_directories(tutikumo_settings_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
target_link_libraries(tutikumo_settings_tests PRIVATE psprecomp_core)
set_target_properties(tutikumo_settings_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_settings_tests COMMAND tutikumo_settings_tests)
endif()

# Where the data directory is (portable or per-user) and the copy of an
# installed copy's data into a portable folder. No game data.
add_executable(tutikumo_data_directory_tests
    tests/data_directory_tests.cpp
    host/install/user_data.cpp
    host/app_paths.cpp
    host/platform/utf8_path.cpp)
target_include_directories(tutikumo_data_directory_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
target_link_libraries(tutikumo_data_directory_tests PRIVATE psprecomp_core)
set_target_properties(tutikumo_data_directory_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_data_directory_tests COMMAND tutikumo_data_directory_tests)
endif()

# Screenshots: their names, and the PNG files written. Works in a temporary
# directory; no game data.
add_executable(tutikumo_screenshot_tests
    tests/screenshot_tests.cpp
    host/gpu/screenshot.cpp
    host/install/user_data.cpp
    host/app_paths.cpp
    host/platform/utf8_path.cpp)
target_include_directories(tutikumo_screenshot_tests PRIVATE
    "${TUTIKUMO_PROFILE_DIR}/host"
    "${TUTIKUMO_PROFILE_DIR}/third_party")
target_link_libraries(tutikumo_screenshot_tests PRIVATE psprecomp_core Threads::Threads)
set_target_properties(tutikumo_screenshot_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_screenshot_tests COMMAND tutikumo_screenshot_tests)
endif()

# Fast loading's decision: when a load runs ahead of real time.
add_executable(tutikumo_fast_loading_tests
    tests/fast_loading_tests.cpp
    host/kernel/load_detector.cpp)
target_include_directories(tutikumo_fast_loading_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
set_target_properties(tutikumo_fast_loading_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_fast_loading_tests COMMAND tutikumo_fast_loading_tests)
endif()

# Fast-forward's switch: hold, toggle, and what keeps real time.
add_executable(tutikumo_fast_forward_tests
    tests/fast_forward_tests.cpp
    host/kernel/fast_forward_switch.cpp)
target_include_directories(tutikumo_fast_forward_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
set_target_properties(tutikumo_fast_forward_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_fast_forward_tests COMMAND tutikumo_fast_forward_tests)
endif()

# The on-screen touch controls: layout, fingers and the stick's maths.
add_executable(tutikumo_touch_tests
    tests/touch_controls_tests.cpp
    host/input/touch_action.cpp
    host/input/touch_controls.cpp)
target_include_directories(tutikumo_touch_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
set_target_properties(tutikumo_touch_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_touch_tests COMMAND tutikumo_touch_tests)
endif()

# Texture pack import: finding a pack in the layouts packs come in, the
# checks made before copying, and the copy, swap and backup; and the PSP's DXT
# block layout. No game data.
add_executable(tutikumo_texture_pack_tests
    tests/texture_pack_tests.cpp
    host/gpu/texture_decode.cpp
    host/gpu/texture_pack.cpp
    host/gpu/texture_pack_import.cpp
    host/platform/utf8_path.cpp)
target_include_directories(tutikumo_texture_pack_tests PRIVATE
    "${TUTIKUMO_PROFILE_DIR}/host"
    "${TUTIKUMO_PROFILE_DIR}/host/gpu"
    "${TUTIKUMO_PROFILE_DIR}/third_party")
target_link_libraries(tutikumo_texture_pack_tests PRIVATE psprecomp_core mhp2g_basisu Threads::Threads)
set_target_properties(tutikumo_texture_pack_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_texture_pack_tests COMMAND tutikumo_texture_pack_tests)
endif()

# Keyboard and mouse bindings: names, settings.ini spelling, the menu's
# assignment rules and what held keys press. No SDL or game data.
add_executable(tutikumo_input_tests
    tests/input_tests.cpp
    host/input/bindings.cpp
    host/input/chords.cpp
    host/input/presets.cpp)
target_include_directories(tutikumo_input_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
set_target_properties(tutikumo_input_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_input_tests COMMAND tutikumo_input_tests)
endif()

# Mappings for gamepads SDL does not know (#147): the setup's detection and
# the mapping lines and file it writes. No SDL.
add_executable(tutikumo_gamepad_mapping_tests
    tests/gamepad_mapping_tests.cpp
    host/input/gamepad_mapping.cpp)
target_include_directories(tutikumo_gamepad_mapping_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
set_target_properties(tutikumo_gamepad_mapping_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_gamepad_mapping_tests COMMAND tutikumo_gamepad_mapping_tests)
endif()

# Mods: the community's mod folders, choices, conflicts and import, the
# DATA.BIN obfuscation and an archive served with mods' files. No game data.
add_executable(tutikumo_mods_tests
    tests/mods_tests.cpp
    host/mods/file_overlay.cpp
    host/mods/tutikumo_data_bin.cpp
    host/mods/tutikumo_mod_format.cpp
    host/mods/mod_import.cpp
    host/mods/mod_ini.cpp
    host/mods/mod_library.cpp
    host/mods/mod_session.cpp)
target_include_directories(tutikumo_mods_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
set_target_properties(tutikumo_mods_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_mods_tests COMMAND tutikumo_mods_tests)
endif()

# Which file holds a piece of equipment's model, read from the game's tables
# as equipment mods need it, on a buffer standing for guest memory. No game data.
add_executable(tutikumo_equipment_models_tests
    tests/equipment_models_tests.cpp
    host/game/game_data.cpp
    host/game/equipment_models.cpp
    host/game/layered_armor.cpp)
target_include_directories(tutikumo_equipment_models_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
set_target_properties(tutikumo_equipment_models_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_equipment_models_tests COMMAND tutikumo_equipment_models_tests)
endif()

# Renderer helpers that need no GPU.
# MMPX, the 2D interface's pixel-art upscaler (host/gpu/mmpx.hpp).
add_executable(tutikumo_mmpx_tests tests/mmpx_tests.cpp host/gpu/mmpx.cpp)
target_include_directories(tutikumo_mmpx_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
set_target_properties(tutikumo_mmpx_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_mmpx_tests COMMAND tutikumo_mmpx_tests)
endif()

# Descriptor pools counted the way a phone's driver counts them: growth,
# freeing, a driver that refuses early, the pool limit. No GPU is used.
if(Vulkan_FOUND)
    add_executable(tutikumo_descriptor_pool_tests tests/descriptor_pool_tests.cpp host/gpu/descriptor_pools.cpp)
    target_include_directories(tutikumo_descriptor_pool_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host"
                                                                     ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(tutikumo_descriptor_pool_tests PRIVATE Vulkan::Vulkan)
    set_target_properties(tutikumo_descriptor_pool_tests PROPERTIES
                          RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(PSPRECOMP_BUILD_TESTS)
        add_test(NAME tutikumo_descriptor_pool_tests COMMAND tutikumo_descriptor_pool_tests)
    endif()
endif()

add_executable(tutikumo_render_tests tests/render_tests.cpp)
target_include_directories(tutikumo_render_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
target_link_libraries(tutikumo_render_tests PRIVATE psprecomp_core)
set_target_properties(tutikumo_render_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_render_tests COMMAND tutikumo_render_tests)
endif()

# Frame interpolation checks: matching draws, cuts, blending, the present
# clock and the frame rate governor.
add_executable(tutikumo_interpolation_tests
    tests/interpolation_tests.cpp
    host/gpu/frame_interpolation.cpp
    host/gpu/frame_pacing.cpp)
target_include_directories(tutikumo_interpolation_tests PRIVATE
    "${TUTIKUMO_PROFILE_DIR}/host"
    "${PROJECT_SOURCE_DIR}/include")
set_target_properties(tutikumo_interpolation_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_interpolation_tests COMMAND tutikumo_interpolation_tests)
endif()

# The built-in ad hoc server against the client and a raw protocol peer, and
# local network discovery, all over the loopback address.
add_executable(tutikumo_adhoc_tests
    tests/adhoc_tests.cpp
    host/adhoc/client.cpp
    host/adhoc/discovery.cpp
    host/adhoc/server.cpp)
target_include_directories(tutikumo_adhoc_tests PRIVATE "${TUTIKUMO_PROFILE_DIR}/host")
target_link_libraries(tutikumo_adhoc_tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(tutikumo_adhoc_tests PRIVATE ws2_32 iphlpapi)
endif()
set_target_properties(tutikumo_adhoc_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_adhoc_tests COMMAND tutikumo_adhoc_tests)
endif()

# Files under folder names outside ASCII: a Cyrillic user folder, a Japanese
# folder for the disc image. Settings, saves, texture packs, fonts, the disc
# image and the executable, in a temporary folder. No game data.
add_executable(tutikumo_path_tests
    tests/path_tests.cpp
    host/app_paths.cpp
    host/fonts/game_font.cpp
    host/gpu/screenshot.cpp
    host/gpu/texture_pack.cpp
    host/gpu/texture_pack_import.cpp
    host/input/bindings.cpp
    host/input/chords.cpp
    host/input/presets.cpp
    host/input/touch_action.cpp
    host/input/touch_controls.cpp
    host/install/user_data.cpp
    host/kernel/iso_image.cpp
    host/platform/utf8_path.cpp
    host/save_data/aes128.cpp
    host/save_data/param_sfo.cpp
    host/save_data/savedata_crypto.cpp
    host/save_data/savedata_store.cpp
    host/save_data/save_transfer.cpp
    host/settings/settings.cpp
    third_party/tiny_aes/aes.c)
target_include_directories(tutikumo_path_tests PRIVATE
    "${TUTIKUMO_PROFILE_DIR}/host"
    "${TUTIKUMO_PROFILE_DIR}/host/gpu"
    "${TUTIKUMO_PROFILE_DIR}/third_party")
target_link_libraries(tutikumo_path_tests PRIVATE psprecomp_core mhp2g_basisu Threads::Threads)
if(WIN32)
    target_link_libraries(tutikumo_path_tests PRIVATE shell32)
endif()
set_target_properties(tutikumo_path_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PSPRECOMP_BUILD_TESTS)
    add_test(NAME tutikumo_path_tests COMMAND tutikumo_path_tests)
endif()

# The host's sources are UTF-8: its text literals (menu text, the Japanese
# sample the font picker checks fonts with) must stay UTF-8 under MSVC, which
# otherwise reads sources in the system's ANSI code page. The recompiled code
# is ASCII and keeps its own flags.
if(MSVC)
    get_property(tutikumo_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS tutikumo_targets)
        if(target STREQUAL "Tutikumo" OR target MATCHES "^tutikumo_.*_tests$")
            target_compile_options(${target} PRIVATE /utf-8)
        endif()
    endforeach()
endif()

# Chained AOT calls nest native frames; give the guest-executing main thread
# the same 64 MiB stack the Windows profiles use.
if(MSVC)
    target_link_options(Tutikumo PRIVATE /STACK:67108864)
elseif(APPLE)
    target_link_options(Tutikumo PRIVATE "LINKER:-stack_size,0x4000000")
endif()

# Recompiled overlay corpora, produced by tools/add_overlay.py. Each directory
# under overlays/ holds one corpus and becomes one shared module that the host
# loads at run time, so a new overlay never relinks the executable.
set(TUTIKUMO_OVERLAY_DIR "${CMAKE_BINARY_DIR}/bin/overlays")

function(tutikumo_add_overlay meta_path)
    get_filename_component(source_dir "${meta_path}" DIRECTORY)
    get_filename_component(TUTIKUMO_OVERLAY_PREFIX "${source_dir}" NAME)
    file(STRINGS "${meta_path}" meta_lines REGEX "^(base|size|code_size|hash|name)=")
    foreach(line IN LISTS meta_lines)
        string(REGEX MATCH "^([a-z_]+)=(.+)$" matched "${line}")
        if(NOT matched)
            continue()
        endif()
        string(TOUPPER "${CMAKE_MATCH_1}" key)
        set(TUTIKUMO_OVERLAY_${key} "${CMAKE_MATCH_2}")
    endforeach()
    if(NOT TUTIKUMO_OVERLAY_BASE OR NOT TUTIKUMO_OVERLAY_SIZE OR NOT TUTIKUMO_OVERLAY_CODE_SIZE
       OR NOT TUTIKUMO_OVERLAY_HASH OR NOT TUTIKUMO_OVERLAY_NAME)
        message(WARNING "tutikumo: incomplete overlay metadata in ${meta_path}")
        return()
    endif()

    file(GLOB sources CONFIGURE_DEPENDS "${source_dir}/*.cpp")
    if(NOT sources)
        message(WARNING "tutikumo: no sources for overlay ${TUTIKUMO_OVERLAY_PREFIX}")
        return()
    endif()
    set(entry_point "${CMAKE_CURRENT_BINARY_DIR}/overlays/${TUTIKUMO_OVERLAY_PREFIX}_module.cpp")
    configure_file("${TUTIKUMO_PROFILE_DIR}/host/overlay_module.cpp.in" "${entry_point}" @ONLY)

    set(target "overlay_${TUTIKUMO_OVERLAY_PREFIX}")
    add_library(${target} MODULE ${sources} "${entry_point}")
    # Headers only: linking psprecomp_core would give the module its own copy of
    # the runtime state the host already owns.
    target_include_directories(${target} PRIVATE
        "${source_dir}" "${TUTIKUMO_PROFILE_DIR}/host" "${PROJECT_SOURCE_DIR}/include")
    if(WIN32 OR APPLE OR TUTIKUMO_ANDROID_APP)
        # Both linkers want the host binary while linking the module: MSVC for
        # its import library, ld64 as the bundle loader. ELF leaves the host
        # symbols undefined and resolves them when the module is loaded. In an
        # Android app the host is libmain.so, which the app loads privately,
        # so the module names it as a dependency to find its symbols.
        target_link_libraries(${target} PRIVATE Tutikumo)
    else()
        add_dependencies(${target} Tutikumo)
        if(ANDROID)
            # The NDK links every shared library with --no-undefined; these
            # leave the host's symbols to the loader like any other ELF.
            target_link_options(${target} PRIVATE "LINKER:-z,undefs")
        endif()
    endif()
    if(MSVC)
        target_compile_definitions(${target} PRIVATE PSPRECOMP_IMPORT_HOST_SYMBOLS=1)
        if(PSPRECOMP_GENERATED_OPT_LEVEL STREQUAL "0")
            target_compile_options(${target} PRIVATE /Od /bigobj)
        else()
            target_compile_options(${target} PRIVATE /O${PSPRECOMP_GENERATED_OPT_LEVEL} /Ob0 /bigobj)
        endif()
    else()
        target_compile_options(${target} PRIVATE -O${PSPRECOMP_GENERATED_OPT_LEVEL} -g0)
    endif()
    if(TUTIKUMO_RELEASE_LINK_OPTIONS)
        target_link_options(${target} PRIVATE ${TUTIKUMO_RELEASE_LINK_OPTIONS})
    endif()
    set_target_properties(${target} PROPERTIES
        JOB_POOL_COMPILE psprecomp_generated
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        PREFIX ""
        SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
        OUTPUT_NAME "${TUTIKUMO_OVERLAY_PREFIX}"
        LIBRARY_OUTPUT_DIRECTORY "${TUTIKUMO_OVERLAY_DIR}")
    if(TUTIKUMO_ANDROID_APP)
        # An APK only carries libraries named lib*.so.
        set_target_properties(${target} PROPERTIES PREFIX "lib")
    endif()
endfunction()

file(GLOB TUTIKUMO_OVERLAY_METADATA CONFIGURE_DEPENDS "${TUTIKUMO_PROFILE_DIR}/overlays/*/meta.txt")
foreach(meta IN LISTS TUTIKUMO_OVERLAY_METADATA)
    tutikumo_add_overlay("${meta}")
endforeach()
list(LENGTH TUTIKUMO_OVERLAY_METADATA TUTIKUMO_OVERLAY_COUNT)
message(STATUS "tutikumo: ${TUTIKUMO_OVERLAY_COUNT} overlay librar(ies) in ${TUTIKUMO_OVERLAY_DIR}")
