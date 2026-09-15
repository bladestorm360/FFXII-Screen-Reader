cmake_minimum_required(VERSION 3.20)
# RC is listed explicitly: src/audio/beacon_assets.rc embeds the beacon sounds as RCDATA.
project(FFXII-Screen-Reader LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- MinHook (inline hooking library, BSD-2 license) ----
# Vendored source under include/MinHook/. Same source layout as DQ7R uses.
add_library(MinHook STATIC
    include/MinHook/hook.c
    include/MinHook/buffer.c
    include/MinHook/trampoline.c
    include/MinHook/hde/hde32.c
    include/MinHook/hde/hde64.c
)
target_include_directories(MinHook PRIVATE
    ${CMAKE_SOURCE_DIR}/include/MinHook
)

# ---- SDL3 (audio output for the navigation beacon; later, controller support) ----
# Built from source and LINKED, the same way DQ7R does it. Audio only — every other
# subsystem is switched off so we are not compiling a renderer we never call.
#
# Consequence to be aware of: this puts a static import on SDL3.dll, so without that DLL
# next to FFXII_TZA.exe the whole mod fails to load — not "no beacon", but no speech and
# no navigation either. That is the accepted trade (the user supplies SDL3.dll the same way
# they supply Tolk.dll); it is called out in README.md's install section because the failure
# mode gives a blind user nothing to go on.
#
# `build_and_deploy.bat` must NOT copy SDL3.dll into the game directory — the user manages
# that install. DQ7R's deploy script does copy it; ours deliberately does not. The RELEASE
# zip does ship it (Docs\release_procedure.md) — deploy, link and release are three separate
# questions, exactly as they are for Tolk.
set(FFXII_SDL3_SOURCE "D:/Games/Dev/SDL3-source" CACHE PATH
    "Path to the SDL3 source tree (release-3.4.4 or newer)")
set(SDL_SHARED       ON  CACHE BOOL "" FORCE)
set(SDL_STATIC       OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL      OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO        OFF CACHE BOOL "" FORCE)
set(SDL_RENDER       OFF CACHE BOOL "" FORCE)
set(SDL_GPU          OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA       OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG       OFF CACHE BOOL "" FORCE)
add_subdirectory("${FFXII_SDL3_SOURCE}" "${CMAKE_BINARY_DIR}/SDL3-build" EXCLUDE_FROM_ALL)

# ---- Main mod DLL (standalone dinput8.dll proxy) ----
# Our DLL ships AS `dinput8.dll` next to FFXII_TZA.exe. Windows' DLL search order
# loads our local copy before searching System32. Inside DllMain we lazy-load the
# real System32\dinput8.dll and forward all exports to it.
add_library(${PROJECT_NAME} SHARED
    src/proxy/dllmain.cpp
    src/proxy/dinput8_proxy.cpp
    src/core/logger.cpp
    src/core/stall_probe.cpp
    src/core/frame_probe.cpp
    src/core/hooks.cpp
    src/core/game_text.cpp
    src/core/message_macro.cpp
    src/core/item_names.cpp
    src/speech/speech.cpp
    src/speech/phrasebook.cpp
    src/speech/phrase_format.cpp
    src/audio/audio_clips.cpp
    src/audio/audio_engine.cpp
    src/audio/beacon_assets.rc
    src/input/input_tracker.cpp
    src/input/pad_hook.cpp
    src/input/pad_router.cpp
    src/input/pad_survey.cpp
    src/ui/text_capture.cpp
    src/ui/menu_state.cpp
    src/ui/config_reader.cpp
    src/ui/menu_reader.cpp
    src/ui/choice_reader.cpp
    src/ui/airship_reader.cpp
    src/ui/gambit_reader.cpp
    src/ui/gambit_picker_reader.cpp
    src/ui/ingame_menu_reader.cpp
    src/ui/char_select_reader.cpp
    src/ui/license_reader.cpp
    src/ui/ability_summary_reader.cpp
    src/ui/shop_reader.cpp
    src/ui/equip_compare.cpp
    src/ui/equip_detail.cpp
    src/ui/equip_target_reader.cpp
    src/ui/inventory_reader.cpp
    src/ui/save_reader.cpp
    src/ui/primer_reader.cpp
    src/ui/gil_reader.cpp
    src/ui/virtual_buffer.cpp
    src/ui/status_reader.cpp
    src/ui/popup_reader.cpp
    src/ui/battle_target_reader.cpp
    src/ui/title_reader.cpp
    src/ui/message_reader.cpp
    src/ui/reward_panel_reader.cpp
    src/ui/dialogue_reader.cpp
    src/ui/mod_menu.cpp
    src/battle/battle_state.cpp
    src/battle/battle_state_diag.cpp
    src/battle/battle_state_names.cpp
    src/battle/party_status.cpp
    src/battle/combat_log.cpp
    src/battle/combat_events.cpp
    src/battle/combat_format.cpp
    src/navigation/navigation.cpp
    src/navigation/player_state.cpp
    src/navigation/bullet_query.cpp
    src/navigation/map_query.cpp
    src/navigation/map_seams.cpp
    src/navigation/map_names.cpp
    src/navigation/map_exits.cpp
    src/navigation/map_script.cpp
    src/navigation/map_script_diag.cpp
    src/navigation/map_script_census.cpp
    src/navigation/nav_hooks.cpp
    src/navigation/nav_common.cpp
    src/navigation/entity_list.cpp
    src/navigation/entity_commands.cpp
    src/navigation/entity_scan.cpp
    src/navigation/entity_postscan.cpp
    src/navigation/exit_scan.cpp
    src/navigation/exit_event_bind.cpp
    src/navigation/item_scan.cpp
    src/navigation/treasure_state.cpp
    src/navigation/entity_classify.cpp
    src/navigation/entity_labels.cpp
    src/navigation/entity_diag.cpp
    src/navigation/interact_target.cpp
    src/navigation/exit_diag.cpp
    src/navigation/nav_commands.cpp
    src/navigation/path_directions.cpp
    src/navigation/nav_mesh.cpp
    src/navigation/nav_footprint.cpp
    src/navigation/nav_probe.cpp
    src/navigation/nav_reach.cpp
    src/navigation/nav_trace.cpp
    src/navigation/path_search.cpp
    src/navigation/path_funnel.cpp
    src/navigation/path_validate.cpp
    src/navigation/path_march.cpp
    src/navigation/path_corridor.cpp
    src/navigation/path_surface_goal.cpp
    src/navigation/path_repair.cpp
    src/navigation/nav_blocked.cpp
    src/navigation/path_danger.cpp
    src/navigation/sneak_assist.cpp
    src/navigation/shout_table.cpp
    src/navigation/shout_gauge.cpp
    src/navigation/shout_diag.cpp
    src/navigation/shout_script.cpp
    src/navigation/shout_fill.cpp
    src/navigation/shout_meter.cpp
    src/navigation/statue_diag.cpp
    src/navigation/statue_table.cpp
    src/navigation/statue_guide.cpp
    src/navigation/path_planner.cpp
    src/navigation/audio_beacon.cpp
    src/navigation/auto_walk.cpp
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "dinput8"
    PREFIX ""
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

# Source files are UTF-8 — tell MSVC explicitly
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

# ---- Build stamp -------------------------------------------------------------------------------
# WHY: no log this mod has ever written carried a version. When a tester reported the combat log
# silent, the only way to work out which build he was running was to COUNT the hook-install lines
# (43 / 52 / 66 across three logs). Every log now names its own build on line 3.
#
# The version string is bumped by hand and must match Docs\release_procedure.md's Release Records.
# It is bumped in a commit made BEFORE the release trigger, not during one -- release_procedure.md
# forbids a build change at release time, and that file's own "bumped at release time" wording is
# what let this rot. THE BUMP IS NOT DONE UNTIL build\CMakeCache.txt AGREES: this is a CACHE STRING,
# so editing the line below on its own is a SILENT NO-OP -- an already-configured cache keeps
# stamping the old number while the source reads the new one, and nothing looks wrong. Update the
# cache too, or delete it, or pass -DFFXII_SR_VERSION=. That trap shipped V0.6 on five releases.
# VERIFY THE BUMP IN THE SHIPPED BINARY, never in the source. The git hash is best-effort: a source
# drop with no .git still builds, it just stamps "nogit".
set(FFXII_SR_VERSION "0.7" CACHE STRING "Mod version, as it appears in the release zip name")

execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short HEAD
    OUTPUT_VARIABLE FFXII_SR_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT FFXII_SR_GIT_HASH)
    set(FFXII_SR_GIT_HASH "nogit")
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    FFXII_SR_VERSION="${FFXII_SR_VERSION}"
    FFXII_SR_GIT_HASH="${FFXII_SR_GIT_HASH}"
)

# Module definition file: declares which symbols this DLL exports (the dinput8 set).
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/proxy/dinput8.def
)

target_link_libraries(${PROJECT_NAME} PRIVATE MinHook Psapi SDL3::SDL3)

# Tolk.dll + nvdaControllerClient64.dll are user-supplied; loaded at runtime via
# LoadLibrary. The build never links them and build_and_deploy.bat never copies them.
# (The release zip DOES ship them — see Docs\release_procedure.md. Deploy, link and
# release are three separate questions; collapsing them once cost a release its TTS DLLs.)
#
# SDL3 is a THIRD case and does not follow either rule above: the build links it, deploy
# still does not copy it, and the release zip must ship it.
