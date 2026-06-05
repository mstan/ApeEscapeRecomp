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
)
