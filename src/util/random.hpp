//
// Created by Kotarou on 2026/7/8.
//

#ifndef YADDNSC_UTIL_RANDOM_H
#define YADDNSC_UTIL_RANDOM_H

#include <cstdint>
#include <random>

#include <openssl/rand.h>

namespace Utils::Random {
    /// Thread-local Mersenne Twister engine (seeded once per thread).
    ///
    /// Seeded from std::random_device on first access.  Suitable for
    /// non-cryptographic shuffling, jitter, and load-balancing needs.
    [[nodiscard]] inline std::mt19937 &engine() {
        thread_local std::mt19937 eng(std::random_device{}());
        return eng;
    }

    /// Cryptographically secure random 16-bit value (OpenSSL RAND_bytes).
    ///
    /// Used for DNS transaction IDs, which must be unpredictable to resist
    /// response spoofing.  Falls back to the thread-local PRNG only if the
    /// CSPRNG fails (essentially never on a healthy system).
    [[nodiscard]] inline std::uint16_t crypto_u16() noexcept {
        std::uint16_t value = 0;
        if (RAND_bytes(reinterpret_cast<unsigned char *>(&value), sizeof(value)) != 1) {
            value = static_cast<std::uint16_t>(engine()());
        }
        return value;
    }
} // namespace Utils::Random

#endif  // YADDNSC_UTIL_RANDOM_H
