//
// Created by Kotarou on 2026/7/13.
//

#include "signing.h"

#include <array>
#include <cstdint>
#include <ctime>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <openssl/evp.h>

// ===========================================================================
//  Internal helpers
// ===========================================================================

namespace {

    // ── RAII deleters for OpenSSL EVP types ──

    struct EvpPKeyDeleter {
        void operator()(EVP_PKEY *pkey) const noexcept { EVP_PKEY_free(pkey); }
    };

    struct EvpMdCtxDeleter {
        void operator()(EVP_MD_CTX *ctx) const noexcept { EVP_MD_CTX_free(ctx); }
    };

    using EvpPKeyPtr = std::unique_ptr<EVP_PKEY, EvpPKeyDeleter>;
    using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

    /// Compute HMAC using EVP_DigestSign (the modern, non-deprecated API).
    [[nodiscard]] std::vector<std::uint8_t> hmac_digest(std::span<const std::uint8_t> key,
                                                         std::span<const std::uint8_t> data,
                                                         const EVP_MD *md) noexcept {
        EvpPKeyPtr pkey(EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, nullptr, key.data(),
                                              static_cast<int>(key.size())));
        if (!pkey)
            return {};

        EvpMdCtxPtr ctx(EVP_MD_CTX_new());
        if (!ctx)
            return {};

        std::vector<std::uint8_t> result;
        if (EVP_DigestSignInit(ctx.get(), nullptr, md, nullptr, pkey.get()) == 1 &&
            EVP_DigestSignUpdate(ctx.get(), data.data(), data.size()) == 1) {
            // First call gets the required length.
            std::size_t len = 0;
            if (EVP_DigestSignFinal(ctx.get(), nullptr, &len) == 1) {
                result.resize(len);
                // Second call produces the actual signature.
                if (EVP_DigestSignFinal(ctx.get(), result.data(), &len) != 1)
                    result.clear();
                else
                    result.resize(len);
            }
        }

        return result;
    }

    /// Compute a one-shot hash digest using the given EVP_MD.
    [[nodiscard]] std::vector<std::uint8_t> hash_digest(std::span<const std::uint8_t> data,
                                                         const EVP_MD *md) noexcept {
        EvpMdCtxPtr ctx(EVP_MD_CTX_new());
        if (!ctx)
            return {};

        std::vector<std::uint8_t> result(EVP_MAX_MD_SIZE, 0);
        unsigned int len = 0;
        EVP_DigestInit_ex(ctx.get(), md, nullptr);
        EVP_DigestUpdate(ctx.get(), data.data(), data.size());
        EVP_DigestFinal_ex(ctx.get(), result.data(), &len);
        result.resize(len);
        return result;
    }

    /// Base64 alphabet (standard, RFC 4648).
    constexpr std::string_view BASE64_CHARS =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /// Hex nibble -> character lookup.
    constexpr std::string_view HEX_CHARS = "0123456789abcdef";

} // anonymous namespace

// ===========================================================================
//  signing::sha256
// ===========================================================================

std::vector<std::uint8_t> Signing::sha256(std::span<const std::uint8_t> data) noexcept {
    return hash_digest(data, EVP_sha256());
}

// ===========================================================================
//  signing::sha1
// ===========================================================================

std::vector<std::uint8_t> Signing::sha1(std::span<const std::uint8_t> data) noexcept {
    return hash_digest(data, EVP_sha1());
}

// ===========================================================================
//  signing::sha256_hex
// ===========================================================================

std::string Signing::sha256_hex(std::string_view data) noexcept {
    const std::vector<std::uint8_t> raw(data.begin(), data.end());
    const auto digest = sha256(std::span<const std::uint8_t>(raw));
    return hex_encode(digest);
}

// ===========================================================================
//  signing::hmac_sha256
// ===========================================================================

std::vector<std::uint8_t> Signing::hmac_sha256(std::span<const std::uint8_t> key,
                                                std::span<const std::uint8_t> data) noexcept {
    return hmac_digest(key, data, EVP_sha256());
}

// ===========================================================================
//  signing::hmac_sha1
// ===========================================================================

std::vector<std::uint8_t> Signing::hmac_sha1(std::span<const std::uint8_t> key,
                                              std::span<const std::uint8_t> data) noexcept {
    return hmac_digest(key, data, EVP_sha1());
}

// ===========================================================================
//  signing::hex_encode
// ===========================================================================

std::string Signing::hex_encode(std::span<const std::uint8_t> data) noexcept {
    if (data.empty())
        return {};

    std::string result;
    result.reserve(data.size() * 2);
    for (const auto byte : data) {
        result.push_back(HEX_CHARS[byte >> 4]);
        result.push_back(HEX_CHARS[byte & 0x0F]);
    }
    return result;
}

// ===========================================================================
//  signing::base64_encode
// ===========================================================================

std::string Signing::base64_encode(std::span<const std::uint8_t> data) noexcept {
    if (data.empty())
        return {};

    // RFC 4648 base64 encoding (no padding).
    const auto input_size = data.size();
    const std::size_t output_size = ((input_size + 2) / 3) * 4;
    std::string result;
    result.reserve(output_size);

    std::size_t i = 0;
    while (i + 3 <= input_size) {
        const auto b0 = data[i];
        const auto b1 = data[i + 1];
        const auto b2 = data[i + 2];
        result.push_back(BASE64_CHARS[b0 >> 2]);
        result.push_back(BASE64_CHARS[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        result.push_back(BASE64_CHARS[((b1 << 2) | (b2 >> 6)) & 0x3F]);
        result.push_back(BASE64_CHARS[b2 & 0x3F]);
        i += 3;
    }

    const auto remaining = input_size - i;
    if (remaining == 1) {
        const auto b0 = data[i];
        result.push_back(BASE64_CHARS[b0 >> 2]);
        result.push_back(BASE64_CHARS[(b0 << 4) & 0x3F]);
    } else if (remaining == 2) {
        const auto b0 = data[i];
        const auto b1 = data[i + 1];
        result.push_back(BASE64_CHARS[b0 >> 2]);
        result.push_back(BASE64_CHARS[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        result.push_back(BASE64_CHARS[(b1 << 2) & 0x3F]);
    }

    return result;
}

// ===========================================================================
//  signing::base64_encode
// ===========================================================================

std::string Signing::iso8601_timestamp() noexcept {
    const auto now = std::time(nullptr);
    const auto *tm = std::gmtime(&now);
    if (!tm)
        return {};
    std::array<char, 24> buf{};
    std::strftime(buf.data(), buf.size(), "%Y%m%dT%H%M%SZ", tm);
    return {buf.data()};
}

// ===========================================================================
//  signing::iso8601_date
// ===========================================================================

std::string Signing::iso8601_date() noexcept {
    const auto now = std::time(nullptr);
    const auto *tm = std::gmtime(&now);
    if (!tm)
        return {};
    std::array<char, 16> buf{};
    std::strftime(buf.data(), buf.size(), "%Y%m%d", tm);
    return {buf.data()};
}
