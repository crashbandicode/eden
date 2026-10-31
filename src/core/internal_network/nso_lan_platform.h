// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace Network::NsoLan {

/// A directory only the current account can use. POSIX: a real directory owned by the current
/// user with mode exactly 0700. Windows: a real directory (no reparse point) owned by the current
/// account or its token's default owner, whose DACL grants access only to that account and SYSTEM.
[[nodiscard]] bool IsPrivateDirectory(const std::string& path);

/// Creates `path` as a private directory, or verifies an existing one. Never follows, replaces or
/// re-permissions a pre-existing symlink, junction or foreign directory.
[[nodiscard]] bool EnsurePrivateDirectory(const std::string& path);

#ifdef _WIN32
/// Creates a new private directory atomically. An existing path is never adopted or modified.
[[nodiscard]] bool CreatePrivateDirectory(const std::string& path);
#endif

} // namespace Network::NsoLan
