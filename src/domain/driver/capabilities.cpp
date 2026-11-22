#include "capabilities.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "domain/dns/record_kind.h"

namespace domain {

DriverCapabilities::DriverCapabilities(std::vector<RecordKind> supported_records)
    : supported_records_(std::move(supported_records)) {}

bool DriverCapabilities::supports(RecordKind kind) const {
    return std::ranges::find(supported_records_, kind) != supported_records_.end();
}

const std::vector<RecordKind>& DriverCapabilities::supported_records() const noexcept {
    return supported_records_;
}

}  // namespace domain
