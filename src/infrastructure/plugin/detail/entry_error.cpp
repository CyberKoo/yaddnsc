#include "entry_error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace plugin::detail {

CreateErrorSnapshot::CreateErrorSnapshot(const yaddnsc_status status, const yaddnsc_error& error,
                                         const std::uint32_t capacity) noexcept {
    valid = yaddnsc_status_is_valid(status) && capacity >= YADDNSC_ERROR_MIN_SIZE &&
            error.struct_size >= YADDNSC_ERROR_MIN_SIZE && error.struct_size <= capacity && error.status == status &&
            yaddnsc_string_is_valid(error.message);
    if (valid) {
        retry = error.retry_after_seconds;
        size = std::min(error.message.size, message.size() - 1);
        if (size != 0) {
            std::memcpy(message.data(), error.message.data, size);
        }
    }
}

void write_entry_error(yaddnsc_error& out_error, const std::string_view message, const yaddnsc_status status) noexcept {
    if (out_error.struct_size < YADDNSC_ERROR_MIN_SIZE) {
        return;
    }
    constexpr std::string_view fallback = "plugin entry threw";
    constexpr std::size_t capacity = 512;
    thread_local std::array<char, capacity> storage{};
    const std::string_view source = message.data() == nullptr ? fallback : message;
    const std::size_t size = std::min(source.size(), storage.size() - 1);
    if (size != 0) {
        std::memcpy(storage.data(), source.data(), size);
    }
    storage[size] = '\0';
    out_error.status = status;
    out_error.retry_after_seconds = 0;
    out_error.message = yaddnsc_string{storage.data(), size};
    out_error.struct_size = out_error.struct_size < static_cast<std::uint32_t>(sizeof(yaddnsc_error))
                                ? out_error.struct_size
                                : static_cast<std::uint32_t>(sizeof(yaddnsc_error));
}

}  // namespace plugin::detail
