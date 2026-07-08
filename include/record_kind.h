//
// Created by Kotarou on 2026/7/3.
//

#ifndef YADDNSC_RECORD_KIND_H
#define YADDNSC_RECORD_KIND_H

/// DNS record kinds supported by the DDNS updater.
///
/// This is a subset of wire-format DNS record types (DNS::RecordType)
/// that the updater can query and update.
enum class RecordKind {
    A,    ///< IPv4 address record
    AAAA, ///< IPv6 address record
    TXT,  ///< Text record
};

#endif // YADDNSC_RECORD_KIND_H
