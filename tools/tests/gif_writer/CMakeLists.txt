# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Standalone test harness for src/yuzu/gif_writer.{h,cpp}. It deliberately does not depend on the
# rest of Eden: only Qt6 Core and Gui (QImage, QSaveFile) plus Python 3 with Pillow.
#
#   cmake -S tools/tests/gif_writer -B build-gifw -DCMAKE_PREFIX_PATH=<Qt6 prefix>
#   cmake --build build-gifw
#   ctest --test-dir build-gifw --output-on-failure
#
# gif_writer_test writes GIFs, the exact source frames and a manifest; validate_gifs.py then
# re-reads them with Pillow and an independent GIF/LZW parser.

cmake_minimum_required(VERSION 3.16)
project(gif_writer_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Core Gui)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

get_filename_component(EDEN_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src" ABSOLUTE)

add_executable(gif_writer_test
    gif_writer_test.cpp
    "${EDEN_SRC_DIR}/yuzu/gif_writer.cpp"
    "${EDEN_SRC_DIR}/yuzu/gif_writer.h"
)
target_include_directories(gif_writer_test PRIVATE "${EDEN_SRC_DIR}")
target_link_libraries(gif_writer_test PRIVATE Qt6::Core Qt6::Gui)
target_compile_definitions(gif_writer_test PRIVATE
    QT_USE_QSTRINGBUILDER QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)

if (MSVC)
    target_compile_options(gif_writer_test PRIVATE /W4 /WX /permissive- /Zc:__cplusplus)
else()
    target_compile_options(gif_writer_test PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wmissing-declarations -Wunused -Werror)
endif()

enable_testing()

set(GIFW_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/gif-output")

add_test(NAME gif_writer_clean
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${GIFW_OUTPUT_DIR}")
add_test(NAME gif_writer_encode
    COMMAND gif_writer_test "${GIFW_OUTPUT_DIR}")
add_test(NAME gif_writer_validate
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_LIST_DIR}/validate_gifs.py" "${GIFW_OUTPUT_DIR}")

set_tests_properties(gif_writer_clean PROPERTIES FIXTURES_SETUP gif_writer_clean)
set_tests_properties(gif_writer_encode PROPERTIES
    FIXTURES_REQUIRED gif_writer_clean
    FIXTURES_SETUP gif_writer_output)
set_tests_properties(gif_writer_validate PROPERTIES FIXTURES_REQUIRED gif_writer_output)
