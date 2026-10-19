//
// support/util — non-cryptographic randomness.
//
// The cryptographic source (OpenSSL RAND_bytes) and its EntropyError live in
// random.hpp; including this header must never expose a CSPRNG-flavoured API,
// and including random.hpp must never expose a predictable one.
//

#ifndef YADDNSC_SUPPORT_UTIL_RANDOM_ENGINE_HPP
#define YADDNSC_SUPPORT_UTIL_RANDOM_ENGINE_HPP

#include <random>

namespace Utils::Random {
/// Thread-local Mersenne Twister engine (seeded once per thread).
///
/// Seeded from std::random_device on first access.  Suitable for
/// non-cryptographic shuffling, jitter, and load-balancing needs.
/// Never use it for query IDs or any other value an adversary influences.
[[nodiscard]] inline std::mt19937& engine() {
    thread_local std::mt19937 eng(std::random_device{}());
    return eng;
}
}  // namespace Utils::Random

#endif  // YADDNSC_SUPPORT_UTIL_RANDOM_ENGINE_HPP
