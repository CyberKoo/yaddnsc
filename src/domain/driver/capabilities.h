#ifndef YADDNSC_DOMAIN_DRIVER_CAPABILITIES_H
#define YADDNSC_DOMAIN_DRIVER_CAPABILITIES_H

#include <vector>

#include "domain/dns/record_kind.h"

namespace domain {

/// The record kinds one driver can publish.
///
/// A domain value type: it carries the capability set a loaded driver
/// advertises and answers the one question the host asks about it — whether a
/// given record kind is publishable. An empty capability set is legal and
/// means the driver supports no record type, so a configuration naming one
/// cannot work.
class DriverCapabilities {
public:
    /// An empty capability set: the driver supports no record type.
    DriverCapabilities() noexcept = default;

    /// Build the capability set from the record kinds the driver supports.
    /// The list may be empty.
    explicit DriverCapabilities(std::vector<RecordKind> supported_records);

    /// Whether the driver can publish @p kind.
    [[nodiscard]] bool supports(RecordKind kind) const;

    /// The record kinds the driver can publish.
    [[nodiscard]] const std::vector<RecordKind>& supported_records() const noexcept;

private:
    std::vector<RecordKind> supported_records_;
};

}  // namespace domain

#endif  // YADDNSC_DOMAIN_DRIVER_CAPABILITIES_H
