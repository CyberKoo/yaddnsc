#include "record_kind.h"

#include <yaddnsc/util/string_util.hpp>

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
    if (yaddnsc::util::iequals(text, "a")) {
        return RecordKind::A;
    }
    if (yaddnsc::util::iequals(text, "aaaa")) {
        return RecordKind::AAAA;
    }
    if (yaddnsc::util::iequals(text, "txt")) {
        return RecordKind::TXT;
    }
    return std::nullopt;
}

}  // namespace domain
