cmake_minimum_required(VERSION 3.20)
project(ape_mouse_gadgets_focused C CXX)
enable_testing()
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(APE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(PSXRECOMP_ROOT "${APE_ROOT}/psxrecomp-v4" CACHE PATH "Reviewed framework checkout")
add_executable(ape_mouse_gadget_stick_test
    "${APE_ROOT}/tests/test_mouse_gadget_stick.cpp"
    "${APE_ROOT}/src/input/ape_mouse_gadget_stick.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/psx_stick.c")
target_include_directories(ape_mouse_gadget_stick_test PRIVATE
    "${APE_ROOT}/src/input" "${PSXRECOMP_ROOT}/runtime/include")
if(NOT MSVC)
    target_link_libraries(ape_mouse_gadget_stick_test PRIVATE m)
endif()
add_test(NAME ape_mouse_gadget_stick_test COMMAND ape_mouse_gadget_stick_test)
add_executable(ape_mouse_gadget_context_test "${APE_ROOT}/tests/test_mouse_gadget_context.cpp")
target_include_directories(ape_mouse_gadget_context_test PRIVATE "${APE_ROOT}/src/input")
add_test(NAME ape_mouse_gadget_context_test COMMAND ape_mouse_gadget_context_test)
add_executable(local_mouse_policy_test
    "${PSXRECOMP_ROOT}/runtime/tests/test_local_mouse_policy.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/local_mouse_policy.cpp")
target_include_directories(local_mouse_policy_test PRIVATE
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/src")
add_test(NAME local_mouse_policy_test COMMAND local_mouse_policy_test)
add_executable(ape_mouse_gadget_activation_test
    "${APE_ROOT}/tests/test_mouse_gadget_activation.cpp"
    "${APE_ROOT}/src/input/ape_mouse_gadget_stick.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/local_mouse_policy.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/psx_stick.c")
target_include_directories(ape_mouse_gadget_activation_test PRIVATE
    "${APE_ROOT}/src/input" "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/runtime/src")
if(NOT MSVC)
    target_link_libraries(ape_mouse_gadget_activation_test PRIVATE m)
endif()
add_test(NAME ape_mouse_gadget_activation_test COMMAND ape_mouse_gadget_activation_test)

add_executable(ape_stick_response_test
    "${APE_ROOT}/tests/test_stick_response.c"
    "${PSXRECOMP_ROOT}/runtime/src/psx_stick.c")
target_include_directories(ape_stick_response_test PRIVATE "${PSXRECOMP_ROOT}/runtime/include")
if(NOT MSVC)
    target_link_libraries(ape_stick_response_test PRIVATE m)
endif()
add_test(NAME ape_stick_response_test COMMAND ape_stick_response_test)

add_executable(ape_preloaded_mods_test
    "${APE_ROOT}/tests/test_preloaded_mods.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/mod_packages.cpp"
    "${PSXRECOMP_ROOT}/runtime/src/crc32.c"
    "${PSXRECOMP_ROOT}/runtime/src/psx_sha256.c")
target_include_directories(ape_preloaded_mods_test PRIVATE
    "${PSXRECOMP_ROOT}/runtime/include" "${PSXRECOMP_ROOT}/recompiler/lib/toml11")
add_test(NAME ape_preloaded_mods_test COMMAND ape_preloaded_mods_test "${APE_ROOT}/mods/preloaded")
