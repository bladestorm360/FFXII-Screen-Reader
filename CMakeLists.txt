cmake_minimum_required(VERSION 3.20)
project(FFXII-Screen-Reader LANGUAGES C CXX)

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

# ---- Main mod DLL (standalone dinput8.dll proxy) ----
# Our DLL ships AS `dinput8.dll` next to FFXII_TZA.exe. Windows' DLL search order
# loads our local copy before searching System32. Inside DllMain we lazy-load the
# real System32\dinput8.dll and forward all exports to it.
# No SDL3 — speech-only; the game's own audio panning covers what an audio-cue
# layer would have done in DQ7R.
add_library(${PROJECT_NAME} SHARED
    src/proxy/dllmain.cpp
    src/proxy/dinput8_proxy.cpp
    src/core/logger.cpp
    src/core/stall_probe.cpp
    src/core/hooks.cpp
    src/core/game_text.cpp
    src/speech/speech.cpp
    src/input/input_tracker.cpp
    src/ui/text_capture.cpp
    src/ui/menu_state.cpp
    src/ui/config_reader.cpp
    src/ui/menu_reader.cpp
    src/ui/ingame_menu_reader.cpp
    src/ui/battle_target_reader.cpp
    src/ui/title_reader.cpp
    src/ui/message_reader.cpp
    src/battle/battle_state.cpp
    src/battle/party_status.cpp
    src/battle/combat_log.cpp
    src/battle/combat_events.cpp
    src/battle/combat_format.cpp
    src/navigation/navigation.cpp
    src/navigation/player_state.cpp
    src/navigation/bullet_query.cpp
    src/navigation/map_query.cpp
    src/navigation/map_names.cpp
    src/navigation/map_exits.cpp
    src/navigation/map_script.cpp
    src/navigation/nav_hooks.cpp
    src/navigation/nav_common.cpp
    src/navigation/entity_list.cpp
    src/navigation/entity_scan.cpp
    src/navigation/entity_diag.cpp
    src/navigation/nav_commands.cpp
    src/navigation/path_directions.cpp
    src/navigation/nav_grid.cpp
    src/navigation/path_planner.cpp
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

# Module definition file: declares which symbols this DLL exports (the dinput8 set).
target_sources(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/proxy/dinput8.def
)

target_link_libraries(${PROJECT_NAME} PRIVATE MinHook Psapi)

# Tolk.dll + nvdaControllerClient64.dll are user-supplied; loaded at runtime via
# LoadLibrary. We deliberately do NOT redistribute them.
