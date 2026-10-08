//
// dns — DNS over TLS.
//

#include "dot.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "infrastructure/dns/bootstrap.h"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/dns_lookup_exception.h"
#include "infrastructure/dns/util.hpp"
#include "infrastructure/dns/validator.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/dns/wire/framing.h"
#include "infrastructure/dns/wire/query_util.h"
#include "infrastructure/net/tls_stream.h"
#include "support/fmt.hpp"
#include "support/util/random.hpp"

namespace dns {
namespace {

/// ALPN identifier for DNS over TLS (RFC 7858 §3.4).
constexpr unsigned char ALPN_DOT[] = {3, 'd', 'o', 't'};

/// RFC 7830 padding block size for a DoT query.
constexpr std::size_t PAD_BLOCK = 128;

/// EDNS0 padding-option overhead excluding the padding bytes themselves:
/// OPT NAME(1) + TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2) + option code(2) +
/// option length(2).
constexpr std::size_t EDNS_PAD_OVERHEAD = 15;

/// Build a query padded to the next 128-octet block (RFC 7830 §3).
[[nodiscard]] std::vector<std::uint8_t> build_padded_query(const std::string& host, const dns::RecordType type) {
    const auto base = dns::QueryBuilder{}.add_question(host, type).build();

    const std::size_t raw_size = base.size() + EDNS_PAD_OVERHEAD;
    const std::size_t pad_length = raw_size % PAD_BLOCK == 0 ? PAD_BLOCK : PAD_BLOCK - raw_size % PAD_BLOCK;

    std::vector<std::uint8_t> padding(pad_length);
    std::ranges::generate(padding, [] { return static_cast<std::uint8_t>(Utils::Random::engine()()); });
    const dns::EdnsOption padding_option{12, padding};  // option 12 = padding

    return dns::QueryBuilder{}.add_question(host, type).add_edns(512, 0, false, std::span(&padding_option, 1)).build();
}

/// Map a transport failure to the DNS vocabulary.
[[nodiscard]] DnsErrorInfo map_connect_error(const net::IoError error) {
    if (error == net::IoError::CANCELLED) {
        return DnsErrorInfo{DnsError::CANCELLED, "DoT connection cancelled"};
    }
    return DnsErrorInfo{DnsError::CONNECTION, "DoT connection failed"};
}

}  // namespace

DotResolver::DotResolver(std::string host, const std::uint16_t port, EndpointOptions options)
    : host_(std::move(host)), port_(port), options_(std::move(options)) {
    options_.tls.alpn_proto = ALPN_DOT;
}

DotResolver::~DotResolver() {
    close();
}

coro::Task<std::expected<std::vector<std::uint8_t>, DnsErrorInfo>> DotResolver::query(std::string host,
                                                                                      const RecordKind kind) {
    // Waiting semantics: a concurrent query queues on the session, it is not
    // refused.
    auto guard = co_await mutex_.lock();
    if (!guard.has_value()) {
        co_return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "DoT session wait cancelled"});
    }

    try {
        const auto record_type = dns::Util::type_to_record_type(kind);
        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id(), host,
                     static_cast<std::uint16_t>(record_type));

        const auto query_bytes = build_padded_query(host, record_type);
        const auto framed = dns::frame_message(query_bytes);
        if (!framed) {
            co_return std::unexpected(DnsErrorInfo{DnsError::PARSE, "DoT query exceeds the framing limit"});
        }

        const std::string label = fmt::format("{}:{}", host_, port_);
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (attempt == 1) {
                SPDLOG_DEBUG(R"(Connection to "{}" failed, reconnecting)", label);
                close();  // rebuild once on a transient failure
            }
            if (auto ready = co_await ensure_stream(); !ready) {
                if (ready.error().code == DnsError::CANCELLED) {
                    co_return std::unexpected(std::move(ready.error()));
                }
                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(std::move(ready.error()));
            }

            auto sent = co_await stream_->send_all(*framed);
            if (!sent) {
                close();
                if (sent.error() == net::IoError::CANCELLED) {
                    co_return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "DoT send cancelled"});
                }
                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(DnsErrorInfo{DnsError::CONNECTION, "DoT send failed"});
            }

            std::array<std::uint8_t, 2> prefix{};
            if (auto got = co_await stream_->read_exact(prefix); !got) {
                close();
                if (got.error() == net::IoError::CANCELLED) {
                    co_return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "DoT receive cancelled"});
                }
                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(DnsErrorInfo{DnsError::CONNECTION, "DoT receive failed"});
            }
            const auto length = dns::read_length(prefix);
            if (!length) {
                close();
                co_return std::unexpected(
                    DnsErrorInfo{DnsError::PARSE, "DoT server announced an invalid message length"});
            }

            std::vector<std::uint8_t> response(*length);
            if (auto got = co_await stream_->read_exact(response); !got) {
                close();
                if (got.error() == net::IoError::CANCELLED) {
                    co_return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "DoT receive cancelled"});
                }
                if (attempt == 0) {
                    continue;
                }
                co_return std::unexpected(DnsErrorInfo{DnsError::CONNECTION, "DoT receive failed"});
            }

            if (auto valid = dns::Validator::validate_response(query_bytes, response); !valid) {
                co_return std::unexpected(std::move(valid.error()));
            }
            SPDLOG_DEBUG(R"(Resolver #{} query succeeded ({} bytes) for "{}")", id(), response.size(), host);
            co_return response;
        }
        co_return std::unexpected(DnsErrorInfo{DnsError::CONNECTION, "DoT query failed"});
    } catch (const std::bad_alloc&) {
        throw;  // allocation failures are never downgraded to a retryable error
    } catch (const DnsLookupException& error) {
        co_return std::unexpected(DnsErrorInfo{error.get_error(), error.what()});
    } catch (const std::exception& error) {
        co_return std::unexpected(
            DnsErrorInfo{DnsError::PARSE, fmt::format(R"(DoT query for "{}" failed: {})", host, error.what())});
    }
}

coro::Task<std::expected<void, DnsErrorInfo>> DotResolver::ensure_stream() {
    if (stream_ != nullptr) {
        auto connected = co_await stream_->ensure_connected();
        if (connected) {
            co_return {};
        }
        co_return std::unexpected(map_connect_error(connected.error()));
    }

    auto addresses = co_await bootstrap_resolve(host_, std::nullopt, options_.bootstrap_dns);
    if (!addresses) {
        co_return std::unexpected(std::move(addresses.error()));
    }

    net::DefaultStreamFactory fallback;
    net::StreamFactory& factory = options_.factory != nullptr ? *options_.factory : fallback;

    DnsErrorInfo last{DnsError::CONNECTION, "no DoT address"};
    for (const InetAddress& address : *addresses) {
        auto stream = factory.create_tls(address, port_, options_.connect, options_.tls);
        auto connected = co_await stream->ensure_connected();
        if (connected) {
            stream_ = std::move(stream);
            co_return {};
        }
        last = map_connect_error(connected.error());
        if (last.code == DnsError::CANCELLED) {
            co_return std::unexpected(std::move(last));
        }
    }
    co_return std::unexpected(std::move(last));
}

void DotResolver::close() noexcept {
    if (stream_ != nullptr) {
        stream_->close();
        stream_.reset();
    }
}

}  // namespace dns
