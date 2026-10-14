// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// MSVC 19.44 corrupts constant-evaluated arrays/maps containing null member-function
// pointers: entries following the first null handler have invalid keys and pointers.
// Initialize these immutable tables once at runtime on MSVC. Lookup remains unchanged.
// GCC/Clang retain constant evaluation. See tools/tests/service_dispatch for a reproducer.
#ifdef _MSC_VER
#define EDEN_SERVICE_TABLE const
#define EDEN_SERVICE_MAP_EVAL
#else
#define EDEN_SERVICE_TABLE constexpr
#define EDEN_SERVICE_MAP_EVAL consteval
#endif
