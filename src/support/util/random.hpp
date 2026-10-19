#ifndef YADDNSC_SUPPORT_UTIL_RANDOM_HPP
#define YADDNSC_SUPPORT_UTIL_RANDOM_HPP

#include <cstdint>
#include <exception>
#include <expected>
#include <openssl/rand.h>

namespace Utils::Random {

/// Entropy acquisition failed. Fixed, allocation-free error data; wire builders
/// may throw this to abort construction, and resolver boundaries translate it.
class EntropyError final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override { return "Cryptographic random source unavailable"; }
};

/// Cryptographically secure random 16-bit value (OpenSSL RAND_bytes).
///
/// Failure: EntropyError if RAND fails; never substitutes a predictable PRNG.
/// RAND may initialize or reseed its generator synchronously on the caller.
[[nodiscard]] inline std::expected<std::uint16_t, EntropyError> crypto_u16() noexcept {
    std::uint16_t value = 0;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&value), sizeof(value)) != 1) {
        return std::unexpected(EntropyError{});
    }
    return value;
}
}  // namespace Utils::Random

#endif  // YADDNSC_SUPPORT_UTIL_RANDOM_HPP
