#ifndef YADDNSC_SDK_SIGNING_HPP
#define YADDNSC_SDK_SIGNING_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/types.h>

/// Cryptographic and encoding utilities for API request signing.
///
/// Provides low-level primitives (SHA-256, SHA-1, HMAC, Base64, hex)
/// used by driver plugins for AWS SigV4, Alibaba Cloud RPC, and similar
/// cloud API signing schemes.
///
/// All functions are stateless and reentrant.  Results allocate dynamic
/// storage, so allocation failures propagate normally rather than causing an
/// unexpected termination through a false noexcept contract.
namespace yaddnsc::sdk::signing {

// ── Byte views ───────────────────────────────────────────────────────────

/// Borrow @p text as raw bytes for the primitives below.
///
/// Zero-copy view: the caller keeps the backing string alive and unmodified for
/// as long as the returned span is used.
[[nodiscard]] inline std::span<const std::uint8_t> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// ── Hash ─────────────────────────────────────────────────────────────────

/// Compute the SHA-256 digest of @p data.
[[nodiscard]] inline std::vector<std::uint8_t> sha256(std::span<const std::uint8_t> data);

/// Compute the SHA-1 digest of @p data.
[[nodiscard]] inline std::vector<std::uint8_t> sha1(std::span<const std::uint8_t> data);

/// Compute the SHA-256 digest and return it as a lowercase hex string.
[[nodiscard]] inline std::string sha256_hex(std::string_view data);

// ── HMAC ─────────────────────────────────────────────────────────────────

/// Compute HMAC-SHA256 of @p data with the given @p key.
[[nodiscard]] inline std::vector<std::uint8_t> hmac_sha256(std::span<const std::uint8_t> key,
                                                           std::span<const std::uint8_t> data);

/// Compute HMAC-SHA1 of @p data with the given @p key.
[[nodiscard]] inline std::vector<std::uint8_t> hmac_sha1(std::span<const std::uint8_t> key,
                                                         std::span<const std::uint8_t> data);

// ── Encoding ─────────────────────────────────────────────────────────────

/// Encode @p data as a lowercase hexadecimal string.
[[nodiscard]] inline std::string hex_encode(std::span<const std::uint8_t> data);

/// Encode @p data as a Base64 string (standard alphabet, no padding).
[[nodiscard]] inline std::string base64_encode(std::span<const std::uint8_t> data);

// ── Time helpers ─────────────────────────────────────────────────────────

/// Current UTC time formatted as "YYYYMMDDTHHmmSSZ".
[[nodiscard]] inline std::string iso8601_timestamp();

/// Current UTC date formatted as "YYYYMMDD".
[[nodiscard]] inline std::string iso8601_date();

}  // namespace yaddnsc::sdk::signing

namespace yaddnsc::sdk::signing::detail {

// ── RAII deleters for OpenSSL EVP types ──

struct EvpPKeyDeleter {
    void operator()(EVP_PKEY* pkey) const noexcept { EVP_PKEY_free(pkey); }
};

struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const noexcept { EVP_MD_CTX_free(ctx); }
};

using EvpPKeyPtr = std::unique_ptr<EVP_PKEY, EvpPKeyDeleter>;
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

/// Compute HMAC using EVP_DigestSign (the modern, non-deprecated API).
[[nodiscard]] inline std::vector<std::uint8_t> hmac_digest(std::span<const std::uint8_t> key,
                                                           std::span<const std::uint8_t> data, const EVP_MD* md) {
    EvpPKeyPtr pkey(EVP_PKEY_new_mac_key(EVP_PKEY_HMAC, nullptr, key.data(), static_cast<int>(key.size())));
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
[[nodiscard]] inline std::vector<std::uint8_t> hash_digest(std::span<const std::uint8_t> data, const EVP_MD* md) {
    EvpMdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx)
        return {};

    std::vector<std::uint8_t> result(EVP_MAX_MD_SIZE, 0);
    unsigned int len = 0;
    if (EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1 || EVP_DigestUpdate(ctx.get(), data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx.get(), result.data(), &len) != 1)
        return {};
    result.resize(len);
    return result;
}

/// Base64 alphabet (standard, RFC 4648).
inline constexpr std::string_view BASE64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Hex nibble -> character lookup.
inline constexpr std::string_view HEX_CHARS = "0123456789abcdef";

}  // namespace yaddnsc::sdk::signing::detail

// ===========================================================================
//  signing::sha256
// ===========================================================================

inline std::vector<std::uint8_t> yaddnsc::sdk::signing::sha256(std::span<const std::uint8_t> data) {
    return detail::hash_digest(data, EVP_sha256());
}

// ===========================================================================
//  signing::sha1
// ===========================================================================

inline std::vector<std::uint8_t> yaddnsc::sdk::signing::sha1(std::span<const std::uint8_t> data) {
    return detail::hash_digest(data, EVP_sha1());
}

// ===========================================================================
//  signing::sha256_hex
// ===========================================================================

inline std::string yaddnsc::sdk::signing::sha256_hex(std::string_view data) {
    const auto digest = sha256(as_bytes(data));
    return hex_encode(digest);
}

// ===========================================================================
//  signing::hmac_sha256
// ===========================================================================

inline std::vector<std::uint8_t> yaddnsc::sdk::signing::hmac_sha256(std::span<const std::uint8_t> key,
                                                                    std::span<const std::uint8_t> data) {
    return detail::hmac_digest(key, data, EVP_sha256());
}

// ===========================================================================
//  signing::hmac_sha1
// ===========================================================================

inline std::vector<std::uint8_t> yaddnsc::sdk::signing::hmac_sha1(std::span<const std::uint8_t> key,
                                                                  std::span<const std::uint8_t> data) {
    return detail::hmac_digest(key, data, EVP_sha1());
}

// ===========================================================================
//  signing::hex_encode
// ===========================================================================

inline std::string yaddnsc::sdk::signing::hex_encode(std::span<const std::uint8_t> data) {
    if (data.empty())
        return {};

    std::string result;
    result.reserve(data.size() * 2);
    for (const auto byte : data) {
        result.push_back(detail::HEX_CHARS[byte >> 4]);
        result.push_back(detail::HEX_CHARS[byte & 0x0F]);
    }
    return result;
}

// ===========================================================================
//  signing::base64_encode
// ===========================================================================

inline std::string yaddnsc::sdk::signing::base64_encode(std::span<const std::uint8_t> data) {
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
        result.push_back(detail::BASE64_CHARS[b0 >> 2]);
        result.push_back(detail::BASE64_CHARS[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        result.push_back(detail::BASE64_CHARS[((b1 << 2) | (b2 >> 6)) & 0x3F]);
        result.push_back(detail::BASE64_CHARS[b2 & 0x3F]);
        i += 3;
    }

    const auto remaining = input_size - i;
    if (remaining == 1) {
        const auto b0 = data[i];
        result.push_back(detail::BASE64_CHARS[b0 >> 2]);
        result.push_back(detail::BASE64_CHARS[(b0 << 4) & 0x3F]);
    } else if (remaining == 2) {
        const auto b0 = data[i];
        const auto b1 = data[i + 1];
        result.push_back(detail::BASE64_CHARS[b0 >> 2]);
        result.push_back(detail::BASE64_CHARS[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        result.push_back(detail::BASE64_CHARS[(b1 << 2) & 0x3F]);
    }

    return result;
}

// ===========================================================================
//  signing::iso8601_timestamp
// ===========================================================================

inline std::string yaddnsc::sdk::signing::iso8601_timestamp() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%dT%H%M%SZ}", now);
}

// ===========================================================================
//  signing::iso8601_date
// ===========================================================================

inline std::string yaddnsc::sdk::signing::iso8601_date() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%d}", now);
}

/// Compatibility alias for existing driver sources.
namespace Signing = yaddnsc::sdk::signing;

#endif  // YADDNSC_SDK_SIGNING_HPP
