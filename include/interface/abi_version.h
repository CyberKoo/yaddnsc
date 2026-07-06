//
// Created by Kotarou on 2026/7/4.
//

#ifndef YADDNSC_ABI_VERSION_H
#define YADDNSC_ABI_VERSION_H

#include <cstdint>

/// Semantic ABI version with backward-compatibility checking.
///
/// Used to verify that a dynamically loaded driver plugin is compatible with
/// the host executable.
///
/// Compatibility rules:
///   - major must match exactly (breaking ABI change, e.g. virtual function
///     removed or signature changed)
///   - driver's minor >= host's required minor (backward-compatible extension,
///     e.g. new virtual function appended)
///   - driver's patch >= host's required patch (implementation change only)
struct AbiVersion final {
    uint16_t major; ///< Breaking ABI version — must match the host exactly
    uint16_t minor; ///< Backward-compatible ABI extension level
    uint16_t patch; ///< Implementation-only change level

    /// Check whether this version is compatible with a required version.
    /// @param required  The minimum ABI version required by the host.
    /// @return true if this version satisfies the compatibility rules.
    [[nodiscard]] constexpr bool is_compatible_with(AbiVersion required) const noexcept {
        return major == required.major &&
               minor >= required.minor &&
               patch >= required.patch;
    }

    /// Equality comparison.
    [[nodiscard]] constexpr bool operator==(AbiVersion other) const noexcept {
        return major == other.major && minor == other.minor && patch == other.patch;
    }
};

#endif //YADDNSC_ABI_VERSION_H
