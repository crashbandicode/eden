// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/internal_network/nso_lan_platform.h"

#ifdef _WIN32

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include "common/common_types.h"
#include "common/logging.h"

namespace Network::NsoLan {

namespace {

/// The process token's user SID and default-owner SID (Administrators for an administrator
/// account; Windows assigns the default owner to every object the process creates).
struct TokenIdentity {
    std::vector<u8> user;
    std::vector<u8> owner;
    bool valid = false;
};

std::vector<u8> TokenSid(HANDLE token, TOKEN_INFORMATION_CLASS what) {
    DWORD size = 0;
    GetTokenInformation(token, what, nullptr, 0, &size);
    if (size == 0) {
        return {};
    }
    std::vector<u8> buffer(size);
    if (!GetTokenInformation(token, what, buffer.data(), size, &size)) {
        return {};
    }
    // TOKEN_USER and TOKEN_OWNER both start with a PSID.
    const PSID sid = what == TokenUser ? reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid
                                       : reinterpret_cast<TOKEN_OWNER*>(buffer.data())->Owner;
    if (sid == nullptr || !IsValidSid(sid)) {
        return {};
    }
    const DWORD length = GetLengthSid(sid);
    std::vector<u8> copy(length);
    std::memcpy(copy.data(), sid, length);
    return copy;
}

const TokenIdentity& Identity() {
    static const TokenIdentity identity = [] {
        TokenIdentity result;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return result;
        }
        result.user = TokenSid(token, TokenUser);
        result.owner = TokenSid(token, TokenOwner);
        CloseHandle(token);
        result.valid = !result.user.empty() && !result.owner.empty();
        return result;
    }();
    return identity;
}

bool SameSid(PSID sid, const std::vector<u8>& other) {
    return !other.empty() && IsValidSid(sid) &&
           EqualSid(sid, const_cast<u8*>(other.data())) != FALSE;
}

bool IsSystemSid(PSID sid) {
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID system = nullptr;
    if (!AllocateAndInitializeSid(&nt, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &system)) {
        return false;
    }
    const bool equal = EqualSid(sid, system) != FALSE;
    FreeSid(system);
    return equal;
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

bool IsRealDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

/// Owner in {token user, token owner, Administrators}; a present DACL whose entries are all ACCESS_ALLOWED,
/// effective (not inherit-only) and for the token user or SYSTEM only.
bool HasPrivateSecurity(const std::wstring& path) {
    const auto& identity = Identity();
    if (!identity.valid) {
        return false;
    }
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                               OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                               &owner, nullptr, &dacl, nullptr, &descriptor);
    if (result != ERROR_SUCCESS) {
        return false;
    }
    const std::unique_ptr<void, decltype(&LocalFree)> release{descriptor, &LocalFree};
    // Elevated creation can assign Administrators as owner. A later unelevated launch by
    // the same account has a different token owner; its exact user/SYSTEM DACL still applies.
    if (owner == nullptr || (!SameSid(owner, identity.user) && !SameSid(owner, identity.owner) &&
                             !IsWellKnownSid(owner, WinBuiltinAdministratorsSid))) {
        return false;
    }
    if (dacl == nullptr) {
        return false; // a missing DACL grants everyone access
    }
    ACL_SIZE_INFORMATION size{};
    if (!GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) || size.AceCount == 0) {
        return false;
    }
    for (DWORD i = 0; i < size.AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace)) {
            return false;
        }
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0) {
            return false;
        }
        PSID sid = &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart;
        if (!SameSid(sid, identity.user) && !IsSystemSid(sid)) {
            return false;
        }
    }
    return true;
}

} // Anonymous namespace

bool IsPrivateDirectory(const std::string& path) {
    const std::wstring wide = Widen(path);
    return !wide.empty() && IsRealDirectory(wide) && HasPrivateSecurity(wide);
}

static bool CreateOrVerifyPrivateDirectory(const std::string& path, bool accept_existing) {
    const std::wstring wide = Widen(path);
    if (wide.empty()) {
        return false;
    }
    if (GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // Pre-existing: never re-permission; it must already be ours and private.
        return accept_existing && IsRealDirectory(wide) && HasPrivateSecurity(wide);
    }
    const auto& identity = Identity();
    if (!identity.valid) {
        return false;
    }
    LPWSTR user_sid = nullptr;
    if (!ConvertSidToStringSidW(const_cast<u8*>(identity.user.data()), &user_sid)) {
        return false;
    }
    // Protected (no inheritance from the parent), inheritable by children: SYSTEM and this account.
    const std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + std::wstring{user_sid} + L")";
    LocalFree(user_sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                              nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const BOOL created = CreateDirectoryW(wide.c_str(), &attributes);
    const DWORD error = GetLastError();
    LocalFree(descriptor);
    if (!created && error == ERROR_ALREADY_EXISTS) {
        return accept_existing && IsRealDirectory(wide) && HasPrivateSecurity(wide);
    }
    if (!created) {
        LOG_ERROR(Network, "CreateDirectory for the private LAN directory failed: {}", error);
        return false;
    }
    if (!IsRealDirectory(wide) || !HasPrivateSecurity(wide)) {
        // Only the empty directory created by this call is eligible for cleanup.
        RemoveDirectoryW(wide.c_str());
        return false;
    }
    return true;
}

bool EnsurePrivateDirectory(const std::string& path) {
    return CreateOrVerifyPrivateDirectory(path, true);
}

bool CreatePrivateDirectory(const std::string& path) {
    return CreateOrVerifyPrivateDirectory(path, false);
}

} // namespace Network::NsoLan

#endif // _WIN32
