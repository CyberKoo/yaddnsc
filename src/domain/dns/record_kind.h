#ifndef YADDNSC_DOMAIN_DNS_RECORD_KIND_H
#define YADDNSC_DOMAIN_DNS_RECORD_KIND_H

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

}  // namespace domain

#endif  // YADDNSC_DOMAIN_DNS_RECORD_KIND_H
