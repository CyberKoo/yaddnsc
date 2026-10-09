#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ABI_STRING_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ABI_STRING_H

#include <string_view>

#include <yaddnsc/sdk/driver_abi.h>

namespace plugin::detail {

/// Borrow ABI text without reading past its explicit length; null data yields
/// an empty view. Callers retain responsibility for ABI validation and lifetime.
[[nodiscard]] inline std::string_view to_view(yaddnsc_string value) noexcept {
    return value.data == nullptr ? std::string_view{} : std::string_view{value.data, value.size};
}

}  // namespace plugin::detail

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ABI_STRING_H
