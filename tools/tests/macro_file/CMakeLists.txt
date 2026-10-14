# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Standalone test harness for src/yuzu/macro_file.{h,cpp}. It deliberately does not depend on the
# rest of Eden: only Qt6 Core plus Python 3 (standard library only).
#
#   cmake -S tools/tests/macro_file -B build-macro -DCMAKE_PREFIX_PATH=<Qt6 prefix>
#   cmake --build build-macro
#   ctest --test-dir build-macro --output-on-failure
#
# macro_file_test checks the API contract and writes real macro files plus a manifest;
# validate_macros.py then re-reads them with Python's json module and checks the on-disk format
# against the written specification independently of the C++ codec.

cmake_minimum_required(VERSION 3.16)
project(macro_file_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(EDEN_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src" ABSOLUTE)

add_executable(macro_file_test
    macro_file_test.cpp
    "${EDEN_SRC_DIR}/yuzu/macro_file.cpp"
    "${EDEN_SRC_DIR}/yuzu/macro_file.h"
)
target_include_directories(macro_file_test PRIVATE "${EDEN_SRC_DIR}")
target_link_libraries(macro_file_test PRIVATE Qt6::Core)
target_compile_definitions(macro_file_test PRIVATE
    QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII
    "EDEN_CONTROL_INPUT_SOURCE=\"${EDEN_SRC_DIR}/yuzu/control_input.cpp\"")

if (MSVC)
    target_compile_options(macro_file_test PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
else()
    target_compile_options(macro_file_test PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wmissing-declarations -Wunused -Werror)
endif()

enable_testing()

set(MACRO_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/macro-output")

add_test(NAME macro_file_clean
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MACRO_OUTPUT_DIR}")
add_test(NAME macro_file_codec
    COMMAND macro_file_test "${MACRO_OUTPUT_DIR}")
add_test(NAME macro_file_validate
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_LIST_DIR}/validate_macros.py" "${MACRO_OUTPUT_DIR}")

set_tests_properties(macro_file_clean PROPERTIES FIXTURES_SETUP macro_file_clean)
set_tests_properties(macro_file_codec PROPERTIES
    FIXTURES_REQUIRED macro_file_clean
    FIXTURES_SETUP macro_file_output)
set_tests_properties(macro_file_validate PROPERTIES FIXTURES_REQUIRED macro_file_output)
