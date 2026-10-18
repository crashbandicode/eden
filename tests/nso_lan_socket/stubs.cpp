// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only definitions of symbols that normally live in unrelated emulator libraries. The
// production socket and the unmodified base network.cpp are compiled and executed for real; only
// the log sink, the assert sink and the host network-interface query are replaced.
#include <array>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging.h"
#include "core/internal_network/network_interface.h"
#include "test_support.h"

namespace {

struct Entry {
    Common::Log::Level level;
    std::string text;
};

std::mutex log_mutex;
std::vector<Entry> log_entries;
std::size_t soft_asserts = 0;

} // Anonymous namespace

namespace Common::Log {

void FmtLogMessageImpl(Class, Level level, const char*, unsigned int, const char*,
                       fmt::string_view format, const fmt::format_args& args) {
    std::string text = fmt::vformat(format, args);
    std::lock_guard lock{log_mutex};
    log_entries.push_back({level, std::move(text)});
}

} // namespace Common::Log

void AssertFailSoftImpl() {
    std::lock_guard lock{log_mutex};
    ++soft_asserts;
}

[[noreturn]] void AssertFatalImpl() {
    std::abort();
}

namespace Network {

std::optional<NetworkInterface> GetSelectedNetworkInterface() {
    return std::nullopt;
}

} // namespace Network

namespace TestSupport {

void ClearLog() {
    std::lock_guard lock{log_mutex};
    log_entries.clear();
}

bool LogContains(Common::Log::Level level, std::string_view text) {
    std::lock_guard lock{log_mutex};
    for (const auto& entry : log_entries) {
        if (entry.level == level && entry.text.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::size_t LogCount(Common::Log::Level level) {
    std::lock_guard lock{log_mutex};
    std::size_t count = 0;
    for (const auto& entry : log_entries) {
        count += entry.level == level ? 1 : 0;
    }
    return count;
}

std::size_t SoftAssertCount() {
    std::lock_guard lock{log_mutex};
    return soft_asserts;
}

} // namespace TestSupport
