//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_PLUGIN_CRYPTO_SIGNING_H
#define YADDNSC_PLUGIN_CRYPTO_SIGNING_H

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Cryptographic and encoding utilities for API request signing.
///
/// Provides low-level primitives (SHA-256, SHA-1, HMAC, Base64, URI encoding)
/// used by driver plugins for AWS SigV4, Alibaba Cloud RPC, and similar
/// cloud API signing schemes.
///
/// All functions are stateless and reentrant.  Results allocate dynamic
/// storage, so allocation failures propagate normally rather than causing an
/// unexpected termination through a false noexcept contract.
namespace Signing {

// ── Hash ─────────────────────────────────────────────────────────────────

/// Compute the SHA-256 digest of @p data.
[[nodiscard]] std::vector<std::uint8_t> sha256(std::span<const std::uint8_t> data);

/// Compute the SHA-1 digest of @p data.
[[nodiscard]] std::vector<std::uint8_t> sha1(std::span<const std::uint8_t> data);

/// Compute the SHA-256 digest and return it as a lowercase hex string.
[[nodiscard]] std::string sha256_hex(std::string_view data);

// ── HMAC ─────────────────────────────────────────────────────────────────

/// Compute HMAC-SHA256 of @p data with the given @p key.
[[nodiscard]] std::vector<std::uint8_t> hmac_sha256(std::span<const std::uint8_t> key,
                                                    std::span<const std::uint8_t> data);

/// Compute HMAC-SHA1 of @p data with the given @p key.
[[nodiscard]] std::vector<std::uint8_t> hmac_sha1(std::span<const std::uint8_t> key,
                                                  std::span<const std::uint8_t> data);

// ── Encoding ─────────────────────────────────────────────────────────────

/// Encode @p data as a lowercase hexadecimal string.
[[nodiscard]] std::string hex_encode(std::span<const std::uint8_t> data);

/// Encode @p data as a Base64 string (standard alphabet, no padding).
[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> data);

// ── Time helpers ─────────────────────────────────────────────────────────

/// Current UTC time formatted as "YYYYMMDDTHHmmSSZ".
[[nodiscard]] std::string iso8601_timestamp();

/// Current UTC date formatted as "YYYYMMDD".
[[nodiscard]] std::string iso8601_date();

}  // namespace Signing

#endif  // YADDNSC_PLUGIN_CRYPTO_SIGNING_H
