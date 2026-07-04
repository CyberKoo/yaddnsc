//
// Created by Kotarou on 2026/7/4.
//

#ifndef YADDNSC_ABI_VERSION_H
#define YADDNSC_ABI_VERSION_H

#include <cstdint>

// ---------------------------------------------------------------------------
// AbiVersion — semantic ABI version with backward-compatibility checking.
//
//   major — breaking ABI change (virtual function removed or signature changed)
//   minor — backward-compatible ABI extension (new virtual function appended)
//   patch — no ABI change, only implementation changes
//
//   Compatibility rules:
//     major must match exactly;
//     driver's minor >= host's required minor;
//     driver's patch >= host's required patch.
// ---------------------------------------------------------------------------
struct AbiVersion final {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;

    [[nodiscard]] constexpr bool is_compatible_with(AbiVersion required) const noexcept {
        return major == required.major &&
               minor >= required.minor &&
               patch >= required.patch;
    }

    [[nodiscard]] constexpr bool operator==(AbiVersion other) const noexcept {
        return major == other.major && minor == other.minor && patch == other.patch;
    }
};

#endif //YADDNSC_ABI_VERSION_H
