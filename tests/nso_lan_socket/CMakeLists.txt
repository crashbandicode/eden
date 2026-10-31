# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone probes for Network::NsoLanSocket; intentionally does not configure or link the
# emulator. Both executables run the production socket and the real Socket/Poll implementation
# from network.cpp. Only the log sink, the assert sink and the host network-interface query are
# replaced (stubs.cpp).
#
#   nso_lan_socket_probe    regression probe against a mock broker on real Unix sockets
#   nso_lan_socket_interop  two real Go runtime children (`nsolan --auto`); registered with ctest
#                           only when -DNSO_LAN_RUNTIME_BINARY=<path to the built runtime> is given
cmake_minimum_required(VERSION 3.22)
project(eden_nso_lan_socket_probe LANGUAGES CXX)

find_package(Threads REQUIRED)
get_filename_component(EDEN_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if (WIN32)
    # Windows probe: the stream transport (nso_lan_socket_win.cpp) against mock_broker_win.h.
    # Header-only json/fmt may come from a CPM cache (-DNSO_LAN_JSON_INCLUDE=... -DNSO_LAN_FMT_INCLUDE=...)
    # when no installed CMake packages exist.
    add_executable(nso_lan_socket_probe_win
        socket_probe_win.cpp
        stubs.cpp
        "${EDEN_SOURCE_ROOT}/src/core/internal_network/nso_lan_socket.cpp"
        "${EDEN_SOURCE_ROOT}/src/core/internal_network/nso_lan_socket_win.cpp"
        "${EDEN_SOURCE_ROOT}/src/core/internal_network/nso_lan_platform_win.cpp"
        "${EDEN_SOURCE_ROOT}/src/core/internal_network/network.cpp"
        "${EDEN_SOURCE_ROOT}/src/core/internal_network/nso_lan.cpp"
        "${EDEN_SOURCE_ROOT}/src/common/error.cpp")
    target_compile_features(nso_lan_socket_probe_win PRIVATE cxx_std_20)
    target_include_directories(nso_lan_socket_probe_win PRIVATE "${EDEN_SOURCE_ROOT}/src")
    # NOMINMAX / WIN32_LEAN_AND_MEAN match the emulator's global Windows definitions.
    target_compile_definitions(nso_lan_socket_probe_win PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 FMT_HEADER_ONLY NOMINMAX WIN32_LEAN_AND_MEAN)
    if (NSO_LAN_JSON_INCLUDE AND NSO_LAN_FMT_INCLUDE)
        target_include_directories(nso_lan_socket_probe_win PRIVATE "${NSO_LAN_JSON_INCLUDE}" "${NSO_LAN_FMT_INCLUDE}")
    else()
        find_package(nlohmann_json CONFIG REQUIRED)
        find_package(fmt CONFIG REQUIRED)
        target_link_libraries(nso_lan_socket_probe_win PRIVATE nlohmann_json::nlohmann_json fmt::fmt-header-only)
    endif()
    if (MSVC)
        target_compile_options(nso_lan_socket_probe_win PRIVATE /utf-8 /EHsc /permissive- /Zc:__cplusplus /bigobj /W4)
    endif()
    target_link_libraries(nso_lan_socket_probe_win PRIVATE Threads::Threads ws2_32 advapi32)
    enable_testing()
    add_test(NAME nso_lan_socket_regressions_win COMMAND nso_lan_socket_probe_win)
    return()
endif()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

set(NSO_LAN_SOCKET_COMMON_SOURCES
    stubs.cpp
    "${EDEN_SOURCE_ROOT}/src/core/internal_network/nso_lan_socket.cpp"
    "${EDEN_SOURCE_ROOT}/src/core/internal_network/network.cpp"
    # network.cpp consults the LAN configuration; it is dependency-free apart from json.
    "${EDEN_SOURCE_ROOT}/src/core/internal_network/nso_lan.cpp"
    "${EDEN_SOURCE_ROOT}/src/common/error.cpp")

foreach(target nso_lan_socket_probe nso_lan_socket_interop)
    if(target STREQUAL "nso_lan_socket_probe")
        add_executable(${target} socket_probe.cpp ${NSO_LAN_SOCKET_COMMON_SOURCES})
    else()
        add_executable(${target} interop_probe.cpp ${NSO_LAN_SOCKET_COMMON_SOURCES})
    endif()
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${EDEN_SOURCE_ROOT}/src")
    target_link_libraries(${target} PRIVATE
        Threads::Threads nlohmann_json::nlohmann_json fmt::fmt-header-only)
endforeach()

enable_testing()
add_test(NAME nso_lan_socket_regressions COMMAND nso_lan_socket_probe)
if(NSO_LAN_RUNTIME_BINARY)
    add_test(NAME nso_lan_socket_go_interop
        COMMAND nso_lan_socket_interop --runtime-binary "${NSO_LAN_RUNTIME_BINARY}")
endif()
