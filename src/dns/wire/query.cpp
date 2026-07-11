//
// Created by Kotarou on 2026/6/29.
//

#include "dns/wire/query.h"

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "dns/wire/builder.h"
#include "exception/dns_lookup.h"

#include "resolver_config.h"

#include "fmt.hpp"
#include <arpa/nameser.h>
#include <resolv.h>

namespace DNS {
    namespace {
        // Buffer size for res_mkquery — must be large enough to accommodate EDNS0 records.
        constexpr size_t MKQUERY_BUFFER_SIZE = 4096;

        // ===================================================================
        //  ns_t_* conversion helper — confined to mkquery_system only.
        //
        //  RecordType and the ns_t_* constants share the same numeric values
        //  (RFC 1035), so a simple static_cast is sufficient.  The explicit
        //  helper documents the intent and keeps the cast in one place.
        // ===================================================================
        [[nodiscard]] int to_ns_type(RecordType type) noexcept {
            // static_cast is safe because RecordType::A == ns_t_a == 1, etc.
            return static_cast<int>(type);
        }
    } // anonymous namespace

    // ===========================================================================
    //  mkquery  —  compile-time dispatch
    //  mkquery_native (EXPERIMENTAL) vs mkquery_system (default).
    // ===========================================================================

    std::vector<std::uint8_t> mkquery(const std::string &host, RecordType type) {
        if constexpr (YADDNSC_USE_NATIVE_DNS) {
            return mkquery_native(host, type);
        } else {
            return mkquery_system(host, type);
        }
    }

    // ===========================================================================
    //  mkquery_system  —  system res_mkquery wrapper
    // ===========================================================================

    std::vector<std::uint8_t> mkquery_system(const std::string &host, RecordType type) {
        // res_mkquery may append EDNS0 records from the system resolver config
        // (e.g. /etc/resolv.conf), so allocate a generous buffer.
        std::vector<std::uint8_t> buf(MKQUERY_BUFFER_SIZE);

        // res_mkquery reads/writes the process-global _res state (e.g. _res.id),
        // so it must be serialized.
        int len = 0;
        static std::mutex res_mutex;
        {
            std::lock_guard lock(res_mutex);
            len = res_mkquery(ns_o_query, host.c_str(), ns_c_in, to_ns_type(type), nullptr, 0, nullptr, buf.data(),
                              static_cast<int>(buf.size()));
        }

        if (len < 0) {
            throw DnsLookupException(fmt::format(R"(Failed to construct DNS query packet for "{}")", host),
                                     DnsError::UNKNOWN);
        }

        buf.resize(static_cast<size_t>(len));
        return buf;
    }

    // ===========================================================================
    //  mkquery_native  —  convenience wrapper around QueryBuilder
    // ===========================================================================

    std::vector<std::uint8_t> mkquery_native(const std::string &host, RecordType type) {
        // EDNS0 (RFC 6891): declare UDP payload size 1232 (RFC 9267).
        // DO bit not set — DNSSEC not requested.
        return QueryBuilder{}
            .add_question(host, type)
            .add_edns(/*udp_payload_size=*/1232)
            .build();
    }
} // namespace DNS
