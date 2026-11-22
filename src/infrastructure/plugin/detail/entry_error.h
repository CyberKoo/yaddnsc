#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_ENTRY_ERROR_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_ENTRY_ERROR_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <yaddnsc/sdk/driver_abi.h>

namespace plugin::detail {

/// Allocation-free firewall error report. The borrowed message remains valid
/// until the next entry error on the same thread.
void write_entry_error(yaddnsc_error& out_error, std::string_view message,
                       yaddnsc_status status = YADDNSC_STATUS_INTERNAL_ERROR) noexcept;

/// Owns no plugin pointers: destroy may overwrite the instance or the
/// thread-local diagnostics that this snapshot preserves.
struct CreateErrorSnapshot {
    std::array<char, 512> message{};
    std::size_t size{0};
    std::uint32_t retry{0};
    bool valid{false};

    CreateErrorSnapshot(yaddnsc_status status, const yaddnsc_error& error, std::uint32_t capacity) noexcept;
};

}  // namespace plugin::detail

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_ENTRY_ERROR_H
