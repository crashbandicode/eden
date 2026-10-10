# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.31)
project(eden_control_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
add_executable(eden-control-fixture control_fixture.cpp ../../yuzu/control_server.cpp)
target_include_directories(eden-control-fixture PRIVATE ../..)
target_link_libraries(eden-control-fixture PRIVATE Qt6::Widgets Qt6::Network)
# Match the definitions and the strictest warnings the yuzu target compiles control_server.cpp
# with (see src/qt_common/CMakeLists.txt), so the fixture catches the same errors.
target_compile_definitions(eden-control-fixture PRIVATE
    QT_USE_QSTRINGBUILDER
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
    QT_NO_URL_CAST_FROM_STRING
    QT_NO_PROCESS_COMBINED_ARGUMENT_START)
target_compile_options(eden-control-fixture PRIVATE -Wall -Wextra -Werror
    -Werror=missing-declarations -Werror=shadow -Werror=unused -Wno-unused-parameter -fno-rtti)
