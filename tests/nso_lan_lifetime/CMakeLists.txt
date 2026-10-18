# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone probe for guest socket descriptor lifetime in the bsd service; intentionally does not
# configure or link the emulator. It compiles the production header-only
# src/core/hle/service/sockets/descriptor_table.h that bsd.cpp uses, against probe sockets on real
# AF_UNIX socketpairs.
#
#   nso_lan_lifetime_probe                 the passing regression suite (registered with ctest)
#   nso_lan_lifetime_probe --legacy-repro  replays the pre-fix bsd.cpp pattern; with
#                                          -DNSO_LAN_LIFETIME_ASAN=ON it must abort with an
#                                          AddressSanitizer heap-use-after-free report
cmake_minimum_required(VERSION 3.22)
project(eden_nso_lan_lifetime_probe LANGUAGES CXX)

option(NSO_LAN_LIFETIME_ASAN "Build with AddressSanitizer" OFF)

find_package(Threads REQUIRED)
get_filename_component(EDEN_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(nso_lan_lifetime_probe lifetime_probe.cpp)
target_compile_features(nso_lan_lifetime_probe PRIVATE cxx_std_20)
target_include_directories(nso_lan_lifetime_probe PRIVATE "${EDEN_SOURCE_ROOT}/src")
target_compile_options(nso_lan_lifetime_probe PRIVATE -Wall -Wextra)
target_link_libraries(nso_lan_lifetime_probe PRIVATE Threads::Threads)
if(NSO_LAN_LIFETIME_ASAN)
    target_compile_options(nso_lan_lifetime_probe PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(nso_lan_lifetime_probe PRIVATE -fsanitize=address)
endif()

enable_testing()
add_test(NAME nso_lan_lifetime_regressions COMMAND nso_lan_lifetime_probe)
