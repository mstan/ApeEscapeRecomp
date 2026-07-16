cmake_minimum_required(VERSION 3.20)
project(ApeEscapeRecomp C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp-v4" CACHE PATH
    "Path to psxrecomp")

include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

set(GAME_GENERATED_FULL
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/SCUS_944.23_full.c")
set(GAME_GENERATED_DISPATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/SCUS_944.23_dispatch.c")

psxrecomp_add_runtime_target(psx-runtime
    GAME_GENERATED_FULL_C "${GAME_GENERATED_FULL}"
    GAME_GENERATED_DISPATCH_C "${GAME_GENERATED_DISPATCH}"
    DEBUG_PORT 4480
    WINDOW_TITLE "ApeEscapeRecomp"
    DEFAULT_BIOS_PATH "${PSXRECOMP_ROOT}/bios/SCPH1001.BIN"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    # Per-game executable name so each title's process is distinct (killing/
    # launching one title by name never hits another's running instance).
    EXE_NAME "ApeEscapeRecomp"
)

# Shared recomp-ui Dear ImGui launcher (prototype, feat/recomp-ui-launcher branch).
# Wires in an alternate PSX-themed launcher UI (see main.cpp RECOMP_LAUNCHER gate)
# instead of the framework's default RmlUi launcher. recomp-ui is vendored via a
# directory junction (recomp-ui -> F:\Projects\recomp-ui) so this stays offline
# and reproducible like everything else in this tree.
include("${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake")
recomp_target_launcher_ui(psx-runtime
    BOXART "${CMAKE_SOURCE_DIR}/recomp/launcher/boxart.tga"
    PAD    "${CMAKE_SOURCE_DIR}/recomp/launcher/pad.tga"
)

# Per-game launcher art override. The framework copies its shared launcher assets
# (img/disc.png etc. — the default is Tomba's disc) next to the exe inside
# psxrecomp_add_runtime_target above; this copies this repo's launcher_art/ on top
# AFTERWARD (POST_BUILD commands run in add order) so the launcher shows Ape
# Escape's disc. img/disc.png is a generated PLACEHOLDER — drop official box/disc
# art into launcher_art/img/ to replace it.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/launcher_art")
    add_custom_command(TARGET psx-runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/launcher_art"
            "$<TARGET_FILE_DIR:psx-runtime>"
        COMMENT "Overriding launcher art with ApeEscapeRecomp/launcher_art")
endif()
