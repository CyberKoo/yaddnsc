//
// Created by Kotarou on 2026/7/8.
//

#include "dns/wire/builder.h"

#include <random>

#include "exception/dns_packet.h"

#include "fmt.hpp"

namespace DNS {
    // ===========================================================================
    //  WireWriter  —  internal DNS wire-format serializer
    // ===========================================================================

    namespace {
        class WireWriter {
        public:
            WireWriter() {
                buf_.reserve(512);
            }

            void write_uint16(std::uint16_t v) {
                buf_.push_back(static_cast<std::uint8_t>(v >> 8));
                buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
            }

            void write_uint32(std::uint32_t v) {
                buf_.push_back(static_cast<std::uint8_t>(v >> 24));
                buf_.push_back(static_cast<std::uint8_t>(v >> 16));
                buf_.push_back(static_cast<std::uint8_t>(v >> 8));
                buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
            }

            // Encode a domain name into DNS label sequence (RFC 1035 §4.1.2).
            //
            // Throws DnsLookupException if:
            //   - any label exceeds 63 octets
            //   - the encoded name exceeds 255 octets
            void encode_name(std::string_view name) {
                if (name.empty() || name == ".") {
                    // Root label only — empty name or bare dot represents the root.
                    buf_.push_back(0);
                    return;
                }

                // Pre-compute the total encoded length to validate early.
                // Encoded name = sum(label_len + 1) per label + 1 (terminating zero).
                // This equals name.size() + label_count + 1.
                size_t label_count = 1;
                for (auto c: name) {
                    if (c == '.')
                        ++label_count;
                }
                const size_t encoded_size = name.size() + label_count + 1;
                if (encoded_size > 255) {
                    throw DnsPacketException(
                        fmt::format("Domain name \"{}\" is too long ({} octets, max 255)", name, encoded_size)
                    );
                }

                size_t pos = 0;
                while (pos < name.size()) {
                    auto dot = name.find('.', pos);
                    if (dot == std::string::npos) {
                        dot = name.size();
                    }

                    const auto label_len = static_cast<std::uint8_t>(dot - pos);
                    if (label_len > 63) {
                        throw DnsPacketException(
                            fmt::format("Label too long in domain name \"{}\" at offset {}", name, pos)
                        );
                    }

                    buf_.push_back(label_len);
                    for (size_t i = 0; i < label_len; ++i) {
                        buf_.push_back(static_cast<std::uint8_t>(name[pos + i]));
                    }
                    pos = dot + 1;
                }

                buf_.push_back(0); // Root label (terminator).
            }

            void write_bytes(std::span<const std::uint8_t> bytes) {
                buf_.insert(buf_.end(), bytes.begin(), bytes.end());
            }

            [[nodiscard]] std::vector<std::uint8_t> finish() && {
                return std::move(buf_);
            }

        private:
            std::vector<std::uint8_t> buf_;
        };

        // Bit positions in the 16-bit DNS flags field (RFC 1035 §4.1.1).
        constexpr std::uint16_t FLAG_QR = 0x8000; // bit 15
        constexpr std::uint16_t FLAG_AA = 0x0400; // bit 10
        constexpr std::uint16_t FLAG_TC = 0x0200; // bit 9
        constexpr std::uint16_t FLAG_RD = 0x0100; // bit 8
        constexpr std::uint16_t FLAG_RA = 0x0080; // bit 7

        // EDNS0 OPT record constants (RFC 6891).
        constexpr std::uint16_t OPT_RR_TYPE = 41;

        /// Build the 16-bit DNS header flags field (RFC 1035 §4.1.1).
        /// @param qr      QR flag (0 = query, 1 = response).
        /// @param opcode  Operation code (4 bits, typically 0 = QUERY).
        /// @param aa      Authoritative Answer flag.
        /// @param tc      Truncation flag.
        /// @param rd      Recursion Desired flag.
        /// @param ra      Recursion Available flag.
        /// @param rcode   Response code (4 bits, 0 = NOERROR).
        /// @return        The packed 16-bit flags field value.
        [[nodiscard]] std::uint16_t
        build_flags(bool qr, std::uint8_t opcode, bool aa, bool tc, bool rd, bool ra, std::uint8_t rcode) noexcept {
            return static_cast<std::uint16_t>((qr ? FLAG_QR : 0) | (static_cast<std::uint16_t>(opcode & 0x0F) << 11) | (
                                                  aa ? FLAG_AA : 0) | (tc ? FLAG_TC : 0) | (rd ? FLAG_RD : 0) | (
                                                  ra ? FLAG_RA : 0) | (rcode & 0x0F));
        }
    } // anonymous namespace

    // ===========================================================================
    //  QueryBuilder  —  public fluent builder
    // ===========================================================================

    QueryBuilder::QueryBuilder() noexcept : id_([] {
                                                static std::random_device rd;
                                                return static_cast<std::uint16_t>(rd() & 0xFFFF);
                                            }()), qr_(false), opcode_(0), aa_(false), tc_(false), rd_(true), ra_(false),
                                            rcode_(0) {
    }

    QueryBuilder &QueryBuilder::id(std::uint16_t id) noexcept {
        id_ = id;
        return *this;
    }

    QueryBuilder &QueryBuilder::qr(bool v) noexcept {
        qr_ = v;
        return *this;
    }

    QueryBuilder &QueryBuilder::opcode(std::uint8_t v) noexcept {
        opcode_ = v;
        return *this;
    }

    QueryBuilder &QueryBuilder::aa(bool v) noexcept {
        aa_ = v;
        return *this;
    }

    QueryBuilder &QueryBuilder::tc(bool v) noexcept {
        tc_ = v;
        return *this;
    }

    QueryBuilder &QueryBuilder::rd(bool v) noexcept {
        rd_ = v;
        return *this;
    }

    QueryBuilder &QueryBuilder::ra(bool v) noexcept {
        ra_ = v;
        return *this;
    }

    QueryBuilder &QueryBuilder::add_question(std::string_view qname, RecordType qtype, RecordClass qclass) {
        questions_.push_back(PendingQuestion{
            .qname = std::string(qname),
            .qtype = qtype,
            .qclass = static_cast<std::uint16_t>(qclass),
        });
        return *this;
    }

    QueryBuilder &QueryBuilder::add_question_raw_qclass(std::string_view qname, RecordType qtype,
                                                        std::uint16_t raw_qclass) {
        questions_.push_back(PendingQuestion{
            .qname = std::string(qname),
            .qtype = qtype,
            .qclass = raw_qclass,
        });
        return *this;
    }

    QueryBuilder &QueryBuilder::add_edns(std::uint16_t udp_payload_size, std::uint8_t version, bool dnssec_ok,
                                         std::span<const EdnsOption> options) {
        edns_ = EdnsConfig{
            .udp_payload_size = udp_payload_size,
            .version = version,
            .dnssec_ok = dnssec_ok,
            .options = std::vector(options.begin(), options.end()),
        };
        return *this;
    }

    std::vector<std::uint8_t> QueryBuilder::build() const {
        if (questions_.empty()) {
            throw DnsPacketException("Query must have at least one question");
        }

        WireWriter w;

        const auto qdcount = static_cast<std::uint16_t>(questions_.size());
        const auto arcount = edns_.has_value() ? static_cast<std::uint16_t>(1) : std::uint16_t{0};

        // ---- Header (12 bytes) ----
        w.write_uint16(id_);
        w.write_uint16(build_flags(qr_, opcode_, aa_, tc_, rd_, ra_, rcode_));
        w.write_uint16(qdcount);
        w.write_uint16(0); // ANCOUNT
        w.write_uint16(0); // NSCOUNT
        w.write_uint16(arcount);

        // ---- Question section ----
        for (const auto &q: questions_) {
            w.encode_name(q.qname);
            w.write_uint16(static_cast<std::uint16_t>(q.qtype));
            w.write_uint16(q.qclass); // raw 16-bit value (QU bit may be set)
        }

        // ---- Additional section: EDNS0 OPT pseudo-record ----
        if (edns_.has_value()) {
            const auto &edns = *edns_;

            // RFC 6891 §4: EDNS version MUST be 0.
            if (edns.version != 0) {
                throw DnsPacketException(
                    fmt::format("EDNS version {} is not supported (only version 0 is valid)",
                                static_cast<unsigned>(edns.version))
                );
            }

            // RFC 6891 §6.1: UDP payload size MUST be ≥ 512.
            if (edns.udp_payload_size < 512) {
                throw DnsPacketException(
                    fmt::format("EDNS UDP payload size {} is too small (minimum 512)", edns.udp_payload_size)
                );
            }

            // Name: root (single zero byte).
            w.encode_name("");

            // Type: OPT (41).
            w.write_uint16(OPT_RR_TYPE);

            // Class: sender's UDP payload size.
            w.write_uint16(edns.udp_payload_size);

            // TTL: extended RCODE (0) | version | DO bit | Z
            // bits 31-24: extended RCODE (we use header rcode, so 0 here)
            // bits 23-16: version
            // bit 15:     DNSSEC OK (DO)
            // bits 14-0:  Z (must be 0)
            const auto ttl = static_cast<std::uint32_t>(
                (static_cast<std::uint32_t>(edns.version) << 16) |
                (edns.dnssec_ok ? std::uint32_t{0x8000} : std::uint32_t{0})
            );
            w.write_uint32(ttl);

            // RDLENGTH and RDATA (options).
            // Compute total option data size.
            std::uint16_t rdlength = 0;
            for (const auto &opt: edns.options) {
                rdlength += static_cast<std::uint16_t>(4 + opt.data.size());
            }
            w.write_uint16(rdlength);

            // Write each option: code(2) + length(2) + data.
            for (const auto &opt: edns.options) {
                w.write_uint16(opt.code);
                w.write_uint16(static_cast<std::uint16_t>(opt.data.size()));
                w.write_bytes(opt.data);
            }
        }

        return std::move(w).finish();
    }
} // namespace DNS
