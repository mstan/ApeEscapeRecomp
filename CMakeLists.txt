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

    # Release overlay staging: the shard cache and the overlay toolchain.
    #
    # Two things must reach a release for this title's overlays to run native on
    # a player's machine -- cache/<id>/<compiler>/<arch-abi>/<tag>/*.dll, and
    # overlay_toolchain/python/python.exe, which is the exact file the runtime
    # gates autocompile on (main.cpp tk_present). Omitting either fails NOTHING
    # at runtime: the loader falls back to the dirty-RAM interpreter and reports
    # itself healthy. That is how this title shipped its entire release history
    # with neither while game.toml promised an AOT cache.
    #
    # The test has two halves. The CONTRACT half always runs and needs no build
    # products: it asserts tools/package_release.ps1 delegates to the framework's
    # shared staging module and keeps no private copy of anything the module owns
    # (a locally formatted cache tag, an unpinned toolchain download, a warning
    # where a throw belongs, an AllowNoCache escape hatch). Every drift defect
    # this campaign found is a contract violation, so this is the half that
    # catches them cheaply. The ARTIFACT half validates a real staged tree or
    # release zip when one is present -- set PSX_RELEASE_STAGE or PSX_RELEASE_ZIP,
    # or let it discover release-stage/ and *-windows-x64.zip -- and is skipped
    # (loudly, in the log) otherwise, because producing a package needs the
    # game's disc image and a full Release build and so cannot run in a bare
    # checkout.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(
        NAME ape_release_overlay_staging_test
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_overlay_staging.py"
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${PSXRECOMP_ROOT}"
    )
endif()
