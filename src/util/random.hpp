//
// Created by Kotarou on 2026/7/8.
//

#ifndef YADDNSC_UTIL_RANDOM_H
#define YADDNSC_UTIL_RANDOM_H

#include <random>

namespace Utils::Random {

/// Thread-local Mersenne Twister engine (seeded once per thread).
///
/// Seeded from std::random_device on first access.  Suitable for
/// non-cryptographic shuffling, jitter, and load-balancing needs.
inline std::mt19937 &engine() {
    thread_local std::mt19937 eng(std::random_device{}());
    return eng;
}

}  // namespace Utils::Random

#endif  // YADDNSC_UTIL_RANDOM_H
