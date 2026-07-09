//
// Created by Kotarou on 2026/7/8.
//

#ifndef YADDNSC_DNS_WIRE_BUILDER_H
#define YADDNSC_DNS_WIRE_BUILDER_H

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dns/types.h"

namespace DNS {
    /// Fluent DNS packet builder (wire format, RFC 1035).
    ///
    /// Constructs arbitrary DNS query/update packets with full control over
    /// header flags, multiple questions, and optional EDNS0 (RFC 6891).
    ///
    /// Default state produces a standard recursive query with a random
    /// transaction ID:
    ///
    /// @code
    ///   auto packet = QueryBuilder{}
    ///       .add_question("example.com", RecordType::A)
    ///       .build();
    /// @endcode
    ///
    /// For full control:
    /// @code
    ///   auto packet = QueryBuilder{}
    ///       .id(0x1234)
    ///       .rd(true)
    ///       .add_question("example.com", RecordType::AAAA)
    ///       .add_question("example.com", RecordType::A)
    ///       .add_edns(4096, 0, true)
    ///       .build();
    /// @endcode
    class QueryBuilder {
    public:
        /// Default-construct with sensible query defaults:
        ///   - ID: random (std::random_device)
        ///   - QR: 0 (query)
        ///   - OPCODE: 0 (standard query)
        ///   - RD: true (recursion desired)
        ///   - All other flags: false
        QueryBuilder() noexcept;

        // ── Header flags ──────────────────────────────────────────────

        /// Set the 16-bit transaction ID.
        QueryBuilder &id(std::uint16_t id) noexcept;

        /// QR — 0 = query, 1 = response.
        QueryBuilder &qr(bool v) noexcept;

        /// OPCODE — 0 = standard query, 1 = inverse, 2 = server status, etc.
        QueryBuilder &opcode(std::uint8_t v) noexcept;

        /// AA — Authoritative Answer.
        QueryBuilder &aa(bool v) noexcept;

        /// TC — TrunCation.
        QueryBuilder &tc(bool v) noexcept;

        /// RD — Recursion Desired.
        QueryBuilder &rd(bool v) noexcept;

        /// RA — Recursion Available.
        QueryBuilder &ra(bool v) noexcept;

        // ── Question section ──────────────────────────────────────────

        /// Append a question to the question section with a typed RecordClass.
        ///
        /// @param qname   The domain name (e.g. "example.com").
        /// @param qtype   The record type to query.
        /// @param qclass  The class (default: IN).
        QueryBuilder &add_question(std::string_view qname, RecordType qtype, RecordClass qclass = RecordClass::IN);

        /// Append a question with a raw 16-bit QCLASS value.
        ///
        /// Useful for protocols that encode extra flags in the QCLASS field,
        /// such as the mDNS QU (unicast-response) bit (RFC 6762 §18.3).
        /// Prefer add_question() with RecordClass for standard usage.
        QueryBuilder &add_question_raw_qclass(std::string_view qname, RecordType qtype, std::uint16_t raw_qclass);

        // ── EDNS0 (RFC 6891) ─────────────────────────────────────────

        /// Attach an EDNS0 OPT pseudo-record to the additional section.
        ///
        /// Calling this method increments ARCOUNT by 1 and writes the OPT
        /// record after all regular sections.
        ///
        /// @param udp_payload_size  Sender's UDP payload size (e.g. 1232, 4096).
        /// @param version           EDNS version (must be 0 for RFC compliance).
        /// @param dnssec_ok         Set the DNSSEC OK (DO) bit.
        /// @param options           Optional EDNS options (ECS, padding, etc.).
        QueryBuilder &add_edns(std::uint16_t udp_payload_size, std::uint8_t version = 0, bool dnssec_ok = false,
                               std::span<const EdnsOption> options = {});

        // ── Build ─────────────────────────────────────────────────────

        /// Produce the wire-format DNS packet.
        ///
        /// The returned buffer contains the complete DNS message ready for
        /// transmission over UDP or TCP.
        ///
        /// @throws DnsPacketException if any input violates DNS protocol constraints
        ///         (empty question section, label > 63 octets, name > 255 octets,
        ///         EDNS version != 0, UDP payload size < 512).
        [[nodiscard]] std::vector<std::uint8_t> build() const;

    private:
        struct EdnsConfig {
            std::uint16_t udp_payload_size;
            std::uint8_t version;
            bool dnssec_ok;
            std::vector<EdnsOption> options;
        };

        struct PendingQuestion {
            std::string qname;
            RecordType qtype;
            std::uint16_t qclass; // raw 16-bit class value (allows QU bit, etc.)
        };

        std::uint16_t id_;
        bool qr_;
        std::uint8_t opcode_;
        bool aa_;
        bool tc_;
        bool rd_;
        bool ra_;
        std::uint8_t rcode_; // only meaningful for responses

        std::vector<PendingQuestion> questions_;
        std::optional<EdnsConfig> edns_;
    };
} // namespace DNS

#endif  // YADDNSC_DNS_WIRE_BUILDER_H
