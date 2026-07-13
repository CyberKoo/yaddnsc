//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_SIGNING_H
#define YADDNSC_SIGNING_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "yaddnsc_export.h"

/// Cryptographic and encoding utilities for API request signing.
///
/// Provides low-level primitives (SHA-256, SHA-1, HMAC, Base64, URI encoding)
/// used by driver plugins for AWS SigV4, Alibaba Cloud RPC, and similar
/// cloud API signing schemes.
///
/// All functions are noexcept, stateless, and reentrant.
namespace Signing {

// ── Hash ─────────────────────────────────────────────────────────────────

/// Compute the SHA-256 digest of @p data.
[[nodiscard]] YADDNSC_EXPORT std::vector<std::uint8_t> sha256(std::span<const std::uint8_t> data) noexcept;

/// Compute the SHA-1 digest of @p data.
[[nodiscard]] YADDNSC_EXPORT std::vector<std::uint8_t> sha1(std::span<const std::uint8_t> data) noexcept;

/// Compute the SHA-256 digest and return it as a lowercase hex string.
[[nodiscard]] YADDNSC_EXPORT std::string sha256_hex(std::string_view data) noexcept;

// ── HMAC ─────────────────────────────────────────────────────────────────

/// Compute HMAC-SHA256 of @p data with the given @p key.
[[nodiscard]] YADDNSC_EXPORT std::vector<std::uint8_t> hmac_sha256(std::span<const std::uint8_t> key,
                                                                     std::span<const std::uint8_t> data) noexcept;

/// Compute HMAC-SHA1 of @p data with the given @p key.
[[nodiscard]] YADDNSC_EXPORT std::vector<std::uint8_t> hmac_sha1(std::span<const std::uint8_t> key,
                                                                   std::span<const std::uint8_t> data) noexcept;

// ── Encoding ─────────────────────────────────────────────────────────────

/// Encode @p data as a lowercase hexadecimal string.
[[nodiscard]] YADDNSC_EXPORT std::string hex_encode(std::span<const std::uint8_t> data) noexcept;

/// Encode @p data as a Base64 string (standard alphabet, no padding).
[[nodiscard]] YADDNSC_EXPORT std::string base64_encode(std::span<const std::uint8_t> data) noexcept;

// ── Time helpers ─────────────────────────────────────────────────────────

/// Current UTC time formatted as "YYYYMMDDTHHmmSSZ".
[[nodiscard]] YADDNSC_EXPORT std::string iso8601_timestamp() noexcept;

/// Current UTC date formatted as "YYYYMMDD".
[[nodiscard]] YADDNSC_EXPORT std::string iso8601_date() noexcept;

} // namespace Signing

#endif // YADDNSC_SIGNING_H
