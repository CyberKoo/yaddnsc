#ifndef YADDNSC_DOMAIN_DNS_RECORD_KIND_H
#define YADDNSC_DOMAIN_DNS_RECORD_KIND_H

#include <optional>
#include <string_view>

namespace domain {

/// DNS record kinds supported by the DDNS updater.
///
/// This is a subset of wire-format DNS record types (dns::RecordType)
/// that the updater can query and update.
enum class RecordKind {
    A,     ///< IPv4 address record
    AAAA,  ///< IPv6 address record
    TXT,   ///< Text record
};

/// Convert a RecordKind to its conventional mnemonic ("A", "AAAA", "TXT").
/// An out-of-range value renders as "UNKNOWN" so a log line still reads.
[[nodiscard]] std::string_view record_kind_to_str(RecordKind kind);

/// Parse a record-kind mnemonic, case-insensitively ("a", "AAAA", "txt", ...).
/// @return the kind, or nullopt when the text names no supported kind.
[[nodiscard]] std::optional<RecordKind> record_kind_from_str(std::string_view text);

}  // namespace domain

#endif  // YADDNSC_DOMAIN_DNS_RECORD_KIND_H
