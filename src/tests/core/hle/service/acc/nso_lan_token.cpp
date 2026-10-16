// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone fixture probe; compile with nso_lan_token.cpp and link OpenSSL::Crypto.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include "core/hle/service/acc/nso_lan_token.h"

namespace {
int checks{};
void Check(bool condition) {
    ++checks;
    if (!condition) {
        throw std::runtime_error("fixture check failed at " + std::to_string(checks));
    }
}

std::string Decode(std::string text) {
    std::replace(text.begin(), text.end(), '-', '+');
    std::replace(text.begin(), text.end(), '_', '/');
    while (text.size() % 4 != 0) {
        text += '=';
    }
    std::string result(text.size(), '\0');
    const int decoded = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                                       reinterpret_cast<const unsigned char*>(text.data()),
                                       static_cast<int>(text.size()));
    Check(decoded >= 0);
    std::size_t padding{};
    for (auto i = text.rbegin(); i != text.rend() && *i == '='; ++i) {
        ++padding;
    }
    result.resize(static_cast<std::size_t>(decoded) - padding);
    return result;
}

bool Verify(const Service::Account::NsoLan::IdToken& token, std::string value) {
    const auto split = value.rfind('.');
    const auto signature = Decode(value.substr(split + 1));
    value.resize(split);
    const auto* der = token.public_key_der.data();
    const std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{
        d2i_PUBKEY(nullptr, &der, static_cast<long>(token.public_key_der.size())), EVP_PKEY_free};
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{
        EVP_MD_CTX_new(), EVP_MD_CTX_free};
    Check(key && context && der == token.public_key_der.data() + token.public_key_der.size());
    Check(EVP_PKEY_bits(key.get()) == 2048);
    Check(EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) == 1);
    return EVP_DigestVerify(context.get(),
                            reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                            reinterpret_cast<const unsigned char*>(value.data()), value.size()) == 1;
}

nlohmann::json Claims(const std::string& token) {
    const auto first = token.find('.');
    const auto second = token.find('.', first + 1);
    Check(first != std::string::npos && second != std::string::npos);
    Check(token.find('.', second + 1) == std::string::npos);
    return nlohmann::json::parse(Decode(token.substr(first + 1, second - first - 1)));
}
} // namespace

int main() {
    using Service::Account::NsoLan::CreateIdToken;
    constexpr std::int64_t now = 1'792'157'000;
    const std::string version = "13.0.4\"\nfixture";
    const auto a = CreateIdToken(49153, now, version, 0x01006a800016e000ULL);
    const auto b = CreateIdToken(49154, now + 9, {}, 0x0100000000000001ULL);
    Check(a.has_value() && b.has_value());
    const auto header = nlohmann::json::parse(Decode(a->value.substr(0, a->value.find('.'))));
    const auto ca = Claims(a->value);
    const auto cb = Claims(b->value);
    Check(header["alg"] == "RS256" && header["typ"] == "id_token");
    Check(!header.contains("jku") && !ca.contains("jku") && !ca.contains("nnex"));
    Check(ca["sub"] == "49153" && ca["npid"] == "49153");
    Check(cb["sub"] == "49154" && cb["npid"] == "49154");
    Check(ca["di"] != cb["di"] && ca["bs:did"] != cb["bs:did"]);
    Check(ca["hm"] == true && ca["eden_fixture"] == true);
    Check(ca["nintendo"]["di"] == ca["di"]);
    Check(ca["iat"] == now && ca["exp"] == now + 1200 && a->expires_at == now + 1200);
    Check(ca["tv"] == version && !cb.contains("tv"));
    Check(ca["app_id"] == "01006a800016e000" && cb["app_id"] == "0100000000000001");
    Check(ca["aud"] == "ed9e2f05d286f7b8");
    Check(a->value.size() < 0xc00 && b->value.size() < 0xc00);
    Check(Verify(*a, a->value));
    Check(Verify(*b, b->value));
    Check(!Verify(*a, b->value));
    auto tampered = a->value;
    tampered[5] = tampered[5] == 'A' ? 'B' : 'A';
    Check(!Verify(*a, tampered));
    const auto renewed = CreateIdToken(49153, now + 1200, {}, 0x01006a800016e000ULL);
    Check(renewed.has_value());
    const auto cr = Claims(renewed->value);
    Check(cr["sub"] == ca["sub"] && cr["di"] == ca["di"]);
    Check(cr["exp"] == now + 2400 && cr["jti"] != ca["jti"]);
    Check(Verify(*renewed, renewed->value));
    Check(!CreateIdToken(0, now, {}, 1));
    Check(!CreateIdToken(1, -1, {}, 1));
    Check(!CreateIdToken(1, std::numeric_limits<std::int64_t>::max(), {}, 1));
    const auto largest = CreateIdToken(std::numeric_limits<std::uint64_t>::max(), now, {}, 1);
    Check(largest.has_value());
    Check(Claims(largest->value)["sub"] == "18446744073709551615");
    Check(Verify(*largest, largest->value));
    std::cout << checks << " account token checks passed; no token or key material emitted\n";
}
