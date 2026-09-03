cmake_minimum_required(VERSION 3.20)
project(ApeEscapeRecomp C CXX)
include(CTest)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp-v4" CACHE PATH
    "Path to psxrecomp")

include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

# Current psxrecomp emits the large game translation unit as numbered shards
# (see psxrecomp-v4/SPLITGEN_MIGRATION.md). Keep the monolithic fallback for
# older/generated source drops.
file(GLOB GAME_GENERATED_FULL CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/SCUS_944.23_full_[0-9][0-9].c")
list(SORT GAME_GENERATED_FULL)
if(NOT GAME_GENERATED_FULL)
    set(GAME_GENERATED_FULL
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/SCUS_944.23_full.c")
endif()
set(GAME_GENERATED_DISPATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/SCUS_944.23_dispatch.c")

file(GLOB APE_MOD_PLUGIN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mods/*.c")

# Ape Escape's trusted, game-owned mod catalog. Every feature stays disabled
# until selected on the launcher's Mods page.
#
# The FRAMEWORK stages this: PRELOADED_MODS_DIR below hands it to
# psxrecomp_add_runtime_target(), which wipes <exe-dir>/mods/bundled and copies
# both its own mods/builtin/packages and these packages into it, plus
# mods/preloaded/README.md -> mods/README.md. This repo must NOT copy the tree
# itself. It used to, with a hand-written POST_BUILD copy_directory into
# <exe-dir>/mods, and because that named the destination as a STRING, framework
# commit 4cc04be3's rename of the staged catalog from mods/packages to
# mods/bundled broke it without breaking anything a configure, compile or link
# step can see: the four ape.* packages went on landing in mods/packages, which
# neither the launcher nor Add-ModCatalog reads, and the release packager threw
# "Mod catalog is missing package(s) the sources define". Bead beads-eio.3.101.
set(APE_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")

psxrecomp_add_runtime_target(psx-runtime
    GAME_GENERATED_FULL_C "${GAME_GENERATED_FULL}"
    GAME_GENERATED_DISPATCH_C "${GAME_GENERATED_DISPATCH}"
    EXTRAS_SOURCES ${APE_MOD_PLUGIN_SOURCES}
    DEBUG_PORT 4480
    WINDOW_TITLE "ApeEscapeRecomp"
    # No DEFAULT_BIOS_PATH: an absolute build-machine path must never be baked
    # into a release exe (runtime.cmake refuses it and warns). Omitting it takes
    # the framework default "bios/SCPH1001.BIN", which the runtime resolves
    # relative to the exe — and which now also accepts the region-qualified
    # "US-PSX-SCPH1001.BIN" spelling (recompiler/include/bios_rom_alias.h).
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    LAUNCHER_BOXART "${CMAKE_SOURCE_DIR}/recomp/launcher/boxart.tga"
    # Per-game executable name so each title's process is distinct (killing/
    # launching one title by name never hits another's running instance).
    EXE_NAME "ApeEscapeRecomp"
    PRELOADED_MODS_DIR "${APE_PRELOADED_MODS}"
)

if(BUILD_TESTING)
    add_executable(ape_stick_response_test
        tests/test_stick_response.c
        "${PSXRECOMP_ROOT}/runtime/src/psx_stick.c"
    )
    target_include_directories(ape_stick_response_test PRIVATE
        "${PSXRECOMP_ROOT}/runtime/include"
    )
    if(NOT MSVC)
        target_link_libraries(ape_stick_response_test PRIVATE m)
    endif()
    add_test(
        NAME ape_stick_response_test
        COMMAND "$<TARGET_FILE:ape_stick_response_test>"
    )

    add_executable(ape_preloaded_mods_test
        tests/test_preloaded_mods.cpp
        src/mods/ape_frame_smoothing_plugin.c
        "${PSXRECOMP_ROOT}/runtime/src/mod_packages.cpp"
        "${PSXRECOMP_ROOT}/runtime/src/crc32.c"
        "${PSXRECOMP_ROOT}/runtime/src/psx_sha256.c"
    )
    target_include_directories(ape_preloaded_mods_test PRIVATE
        "${PSXRECOMP_ROOT}/runtime/include"
        "${PSXRECOMP_ROOT}/recompiler/lib/toml11"
    )
    add_test(
        NAME ape_preloaded_mods_test
        COMMAND "$<TARGET_FILE:ape_preloaded_mods_test>"
                "${APE_PRELOADED_MODS}"
    )
endif()
