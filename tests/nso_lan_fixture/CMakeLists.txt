# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone probes; intentionally does not configure or link the emulator.
cmake_minimum_required(VERSION 3.22)
project(eden_nso_lan_fixture_probes LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(EDEN_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(nso_lan_config_probe config_probe.cpp
    "${EDEN_SOURCE_ROOT}/src/core/internal_network/nso_lan.cpp")
add_executable(nso_lan_token_probe
    "${EDEN_SOURCE_ROOT}/src/tests/core/hle/service/acc/nso_lan_token.cpp"
    "${EDEN_SOURCE_ROOT}/src/core/hle/service/acc/nso_lan_token.cpp")
foreach(target nso_lan_config_probe nso_lan_token_probe)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${EDEN_SOURCE_ROOT}/src")
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json)
endforeach()
target_link_libraries(nso_lan_token_probe PRIVATE OpenSSL::Crypto)

enable_testing()
add_test(NAME nso_lan_fixture_regressions
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run_probes.py"
        --config-probe "$<TARGET_FILE:nso_lan_config_probe>"
        --token-probe "$<TARGET_FILE:nso_lan_token_probe>")
