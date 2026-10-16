// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/hle/service/acc/nso_lan_token.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <span>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace Service::Account::NsoLan {
namespace {

std::string Encode(std::span<const unsigned char> bytes) {
    if (bytes.empty()) {
        return {};
    }
    std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), bytes.data(),
                    static_cast<int>(bytes.size()));
    while (encoded.back() == '=') {
        encoded.pop_back();
    }
    std::ranges::replace(encoded, '+', '-');
    std::ranges::replace(encoded, '/', '_');
    return encoded;
}

std::string Encode(std::string_view text) {
    return Encode(std::span{reinterpret_cast<const unsigned char*>(text.data()), text.size()});
}

std::string Hex(std::uint64_t value, int width) {
    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(width) << value;
    return result.str();
}

} // namespace

std::optional<IdToken> CreateIdToken(std::uint64_t account_id, std::int64_t now,
                                    std::string_view title_version, std::uint64_t program_id) {
    constexpr std::int64_t lifetime = 20 * 60;
    if (account_id == 0 || now < 0 || now > std::numeric_limits<std::int64_t>::max() - lifetime) {
        return std::nullopt;
    }

    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context{
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};
    EVP_PKEY* raw_key{};
    if (!key_context || EVP_PKEY_keygen_init(key_context.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) != 1 ||
        EVP_PKEY_keygen(key_context.get(), &raw_key) != 1) {
        return std::nullopt;
    }
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{raw_key, EVP_PKEY_free};
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> signing_context{
        EVP_MD_CTX_new(), EVP_MD_CTX_free};
    std::array<unsigned char, 16> nonce{};
    if (!signing_context || RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return std::nullopt;
    }

    // Keep the BAAS parser's claim shape, but supply only fixture identities. In particular,
    // there is no jku directing a guest to Nintendo or any third-party key service.
    const auto device_id = Hex(account_id, 32);
    const nlohmann::json header{{"alg", "RS256"}, {"typ", "id_token"},
                                {"kid", "eden-private-lan-fixture"}};
    nlohmann::json payload{
        {"sub", std::to_string(account_id)}, {"npid", std::to_string(account_id)},
        {"aud", "ed9e2f05d286f7b8"},
        {"iss", "https://e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com"},
        {"typ", "id_token"}, {"iat", now}, {"exp", now + lifetime},
        {"jti", Encode(nonce)}, {"di", device_id}, {"bs:did", device_id},
        {"sn", "XAW10000000000"}, {"hm", true}, {"eden_fixture", true},
        {"app_id", Hex(program_id, 16)},
        {"nintendo", {{"dt", "NX Prod 1"}, {"pc", "HAC"}, {"di", device_id},
                      {"sn", "XAW10000000000"}, {"ist", false}}}};
    if (!title_version.empty()) {
        payload["tv"] = title_version;
    }
    const std::string input = Encode(header.dump()) + "." + Encode(payload.dump());
    std::size_t signature_size{};
    if (EVP_DigestSignInit(signing_context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1 ||
        EVP_DigestSign(signing_context.get(), nullptr, &signature_size,
                       reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 1) {
        return std::nullopt;
    }
    std::vector<unsigned char> signature(signature_size);
    if (EVP_DigestSign(signing_context.get(), signature.data(), &signature_size,
                       reinterpret_cast<const unsigned char*>(input.data()), input.size()) != 1) {
        return std::nullopt;
    }
    signature.resize(signature_size);
    IdToken result{input + "." + Encode(signature), {}, now + lifetime};
    const int public_size = i2d_PUBKEY(key.get(), nullptr);
    if (public_size <= 0) {
        return std::nullopt;
    }
    result.public_key_der.resize(static_cast<std::size_t>(public_size));
    auto* output = result.public_key_der.data();
    if (i2d_PUBKEY(key.get(), &output) != public_size) {
        return std::nullopt;
    }
    return result;
}

} // namespace Service::Account::NsoLan
