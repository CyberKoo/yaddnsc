//
// Classic DNS length framing (RFC 1035 section 4.2.2).
//
// A DNS message sent over TCP carries a two-byte big-endian length prefix
// ahead of the payload. Both classic TCP paths — the resolver fallback and
// the bootstrap exchange — frame and unframe through here, so the wire limit
// and the prefix encoding have one definition.
//
// This header is pure byte manipulation: it performs no I/O and attaches no
// resolver identity or error context. Callers translate FrameError into their
// own DnsErrorInfo.
//

#ifndef YADDNSC_INFRASTRUCTURE_DNS_WIRE_FRAMING_H
#define YADDNSC_INFRASTRUCTURE_DNS_WIRE_FRAMING_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include <arpa/inet.h>

#include <expected>

namespace dns {

/// Largest DNS message accepted on the classic UDP path — the receive buffer
/// size for one datagram. The TCP paths accept up to the 16-bit prefix's
/// 65535-octet maximum instead (RFC 1035 §4.2.2).
inline constexpr std::size_t MAX_MESSAGE_SIZE = 4096;

/// Why a message length could not be framed or read.
enum class FrameError {
    MESSAGE_TOO_LARGE,  ///< payload longer than a 16-bit prefix can announce
    INVALID_LENGTH,     ///< announced length is 0
};

/// Prepend the two-byte big-endian length prefix to @p message.
///
/// @returns MESSAGE_TOO_LARGE when @p message exceeds 65535 bytes.
[[nodiscard]] inline std::expected<std::vector<std::uint8_t>, FrameError> frame_message(
    const std::span<const std::uint8_t> message) {
    if (message.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(FrameError::MESSAGE_TOO_LARGE);
    }

    const auto be_len = htons(static_cast<std::uint16_t>(message.size()));
    std::vector<std::uint8_t> framed(sizeof(be_len) + message.size());
    std::memcpy(framed.data(), &be_len, sizeof(be_len));
    std::memcpy(framed.data() + sizeof(be_len), message.data(), message.size());
    return framed;
}

/// Read the two-byte big-endian length prefix.
///
/// The prefix is 16 bits, so the announced length is inherently bounded by
/// 65535 (RFC 1035 §4.2.2); only a zero length is invalid.
[[nodiscard]] inline std::expected<std::size_t, FrameError> read_length(
    const std::span<const std::uint8_t, 2> prefix) {
    std::uint16_t be_len = 0;
    std::memcpy(&be_len, prefix.data(), sizeof(be_len));
    const std::size_t length = ntohs(be_len);
    if (length == 0) {
        return std::unexpected(FrameError::INVALID_LENGTH);
    }
    return length;
}

}  // namespace dns

#endif  // YADDNSC_INFRASTRUCTURE_DNS_WIRE_FRAMING_H
