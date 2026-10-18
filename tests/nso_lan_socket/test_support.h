// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "common/logging.h"

// Test-side view of the stubbed logging and assert sinks (see stubs.cpp).
namespace TestSupport {

void ClearLog();
bool LogContains(Common::Log::Level level, std::string_view text);
std::size_t LogCount(Common::Log::Level level);
std::size_t SoftAssertCount();

} // namespace TestSupport
