#include "record_kind.h"

#include <algorithm>
#include <cctype>

namespace {

/// ASCII case-insensitive comparison, kept local so the domain layer stays
/// free of library dependencies.
[[nodiscard]] bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

}  // namespace

namespace domain {

std::string_view record_kind_to_str(RecordKind kind) {
    switch (kind) {
        case RecordKind::A:
            return "A";
        case RecordKind::AAAA:
            return "AAAA";
        case RecordKind::TXT:
            return "TXT";
    }
    return "UNKNOWN";
}

std::optional<RecordKind> record_kind_from_str(std::string_view text) {
    if (iequals(text, "a")) {
        return RecordKind::A;
    }
    if (iequals(text, "aaaa")) {
        return RecordKind::AAAA;
    }
    if (iequals(text, "txt")) {
        return RecordKind::TXT;
    }
    return std::nullopt;
}

}  // namespace domain
