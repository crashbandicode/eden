# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Standalone tests for src/yuzu_cmd/nx2_key_script.{h,cpp}. No emulator, SDL, or Qt.
#
#   cmake -S tools/tests/key_script -B /tmp/nx2-key-script-test -DCMAKE_BUILD_TYPE=Release
#   cmake --build /tmp/nx2-key-script-test
#   ctest --test-dir /tmp/nx2-key-script-test --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(nx2_key_script_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(EDEN_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src" ABSOLUTE)

add_executable(nx2_key_script_test
    key_script_test.cpp
    "${EDEN_SRC_DIR}/yuzu_cmd/nx2_key_script.cpp"
    "${EDEN_SRC_DIR}/yuzu_cmd/nx2_key_script.h"
)
target_include_directories(nx2_key_script_test PRIVATE "${EDEN_SRC_DIR}")

if (MSVC)
    target_compile_options(nx2_key_script_test PRIVATE /W4 /WX /permissive- /Zc:__cplusplus)
else()
    target_compile_options(nx2_key_script_test PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wmissing-declarations -Wunused -Werror)
endif()

enable_testing()
add_test(NAME nx2_key_script_test COMMAND nx2_key_script_test)
