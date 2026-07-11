//
// Created by Kotarou on 2026/6/28.
//
#include "doh.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dns/util.hpp"
#include "dns/validator.h"
#include "dns/wire/query.h"
#include "dns/dns_error_info.h"
#include "exception/dns_lookup.h"
#include "exception/tls.h"
#include "network/tls_connection.h"
#include "util/http_parser.h"
#include "dns/resolver_registry.h"

#include "dns_error.h"
#include "uri.h"
#include "version.h"

#include "fmt.hpp"
#include <spdlog/spdlog.h>

namespace {
    using namespace std::chrono_literals;
    constexpr auto DOH_CONTENT_TYPE = "application/dns-message";

    /// Build a proper HTTP Host header value per RFC 7230 §5.4.
    /// Omits the port when it is the HTTPS default (443) and brackets IPv6 addresses.
    [[nodiscard]] std::string build_host_header(std::string_view host, std::uint16_t port) {
        const bool is_ipv6 = host.find(':') != std::string_view::npos;
        if (port == 443) {
            return is_ipv6 ? fmt::format("[{}]", host) : std::string(host);
        }
        return is_ipv6 ? fmt::format("[{}]:{}", host, port) : fmt::format("{}:{}", host, port);
    }
} // anonymous namespace

// ===========================================================================
//  DohResolver::Impl  —  private implementation
// ===========================================================================

struct DohResolver::Impl {
    // ── Constants ──
    static constexpr auto IDLE_TIMEOUT = 30s;
    static constexpr auto CONNECT_TIMEOUT = 1s;
    static constexpr unsigned char ALPN_HTTP[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};

    // ── Constructor ──
    explicit Impl(std::string server, std::uint16_t port, std::string path, std::uint64_t id, std::string label);

    // ── Public member functions ──
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type, int cancel_fd = -1) const;

    // ── Private helpers ──
    /// Ensure a persistent TLS connection exists (create or reuse).
    /// @return  std::expected<void, DnsErrorInfo> — empty on success, error on failure.
    [[nodiscard]] std::expected<void, DnsErrorInfo> ensure_connection() const;

    [[nodiscard]] std::vector<std::uint8_t> build_http_request(std::span<const std::uint8_t> dns_body) const;

    /// Send the HTTP request with one automatic reconnect.
    /// @return  std::expected on success or I/O error (timeout, cancellation).
    [[nodiscard]]
    std::expected<void, DnsErrorInfo> send_request(std::span<const std::uint8_t> request, int cancel_fd) const;

    /// Read and parse the HTTP response.
    /// @return  DNS response body on success, or an I/O/parse error.
    ///          Does NOT throw.
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> read_response(int cancel_fd) const;

    // ── Data members ──
    const std::uint64_t id_;
    const std::string server_;
    const std::uint16_t port_;
    const std::string path_;
    const std::string host_header_;
    const std::string label_;   // display label for log / error messages
    mutable std::mutex mutex_;
    mutable std::unique_ptr<TlsConnection> persistent_conn_;
    mutable std::chrono::steady_clock::time_point last_use_;
};

DohResolver::Impl::Impl(std::string server, std::uint16_t port, std::string path, std::uint64_t id, std::string label)
    : id_(id), server_(std::move(server)), port_(port), path_(std::move(path)),
      host_header_(build_host_header(server_, port_)), label_(std::move(label)), last_use_(std::chrono::steady_clock::now()) {
}

// ===========================================================================
//  Impl::query  —  orchestrator
// ===========================================================================

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DohResolver::Impl::query(
    const std::string &host, RecordKind type, int cancel_fd) const {
    try {
        const auto record_type = DNS::Util::type_to_record_type(type);

        SPDLOG_DEBUG(R"(Resolver #{} lookup for domain "{}" (type {}))", id_, host,
                     static_cast<std::uint16_t>(record_type));

        // ---- 1. Build the raw DNS query packet ----
        const auto query_bytes = DNS::mkquery(host, record_type);

        // ---- 2. Build HTTP POST request (RFC 8484) ----
        const auto http_request = build_http_request(query_bytes);

        // ---- 3. I/O under mutex for shared connection -------
        // Retry once with reconnection on transient I/O failure.
        constexpr int MAX_ATTEMPTS = 2;
        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            std::lock_guard lock(mutex_);

            if (attempt == 1) {
                SPDLOG_DEBUG(R"(Connection to "{}" failed, reconnecting)", label_);
                persistent_conn_->close();
            }

            auto send_result = send_request(http_request, cancel_fd);
            if (!send_result) {
                if (attempt < MAX_ATTEMPTS - 1) continue;
                return std::unexpected(std::move(send_result.error()));
            }

            auto response = read_response(cancel_fd);
            if (!response) {
                // CANCELLED should not be retried — abort immediately.
                if (response.error().code == DnsError::CANCELLED) {
                    return std::unexpected(std::move(response.error()));
                }
                if (attempt < MAX_ATTEMPTS - 1) continue;
                return std::unexpected(std::move(response.error()));
            }

            // ---- 4. Validate DNS response header (RFC 8484 §5.1 / RFC 1035 §4.1.1) ----
            auto valid = DNS::Validator::validate_response(query_bytes, *response);
            if (!valid) {
                return std::unexpected(std::move(valid.error()));
            }

            last_use_ = std::chrono::steady_clock::now();
            SPDLOG_DEBUG(R"(Resolver #{} query succeeded ({} bytes) for "{}")", id_, response->size(), host);

            return std::move(*response);
        }

        // Not reached.
        std::unreachable();
    } catch (const DnsLookupException &e) {
        return std::unexpected(DnsErrorInfo{e.get_error(), e.what()});
    } catch (const std::exception &e) {
        return std::unexpected(DnsErrorInfo{
            DnsError::UNKNOWN,
            fmt::format(R"(Query for "{}" failed: {})", host, e.what())
        });
    }
}

// ===========================================================================
//  Helper implementations
// ===========================================================================

// ---------------------------------------------------------------------------
//  build_http_request  —  constructs the wire-format HTTP POST request
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> DohResolver::Impl::build_http_request(std::span<const std::uint8_t> dns_body) const {
    auto header = fmt::format(
        "POST {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "User-Agent: Mozilla/5.0 (compatible; {})\r\n"
        "Content-Type: {}\r\n"
        "Content-Length: {}\r\n"
        "Accept: {}\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path_, host_header_, YADDNSC::get_full_version(), DOH_CONTENT_TYPE, dns_body.size(), DOH_CONTENT_TYPE);

    std::vector<std::uint8_t> request;
    request.reserve(header.size() + dns_body.size());
    request.insert(request.end(), reinterpret_cast<const std::uint8_t *>(header.data()),
                   reinterpret_cast<const std::uint8_t *>(header.data() + header.size()));
    request.insert(request.end(), dns_body.begin(), dns_body.end());
    return request;
}

// ---------------------------------------------------------------------------
//  send_request  —  sends the HTTP request (single attempt)
//
//  Retry with reconnection is handled at the query() level.
//  Returns std::expected for I/O errors (cancellation, send failure).
// ---------------------------------------------------------------------------

std::expected<void, DnsErrorInfo> DohResolver::Impl::send_request(
    std::span<const std::uint8_t> request, int cancel_fd) const {
    if (auto res = ensure_connection(); !res) {
        return std::unexpected(std::move(res.error()));
    }
    auto status = persistent_conn_->send_all(request, cancel_fd);

    if (!status) {
        if (status.error() == TlsConnection::IoStatus::CANCELLED) {
            persistent_conn_->close();
            return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "Query cancelled"});
        }
        persistent_conn_->close();
        return std::unexpected(DnsErrorInfo{
            DnsError::CONNECTION,
            fmt::format(R"(Failed to send request to "{}")", label_)
        });
    }

    SPDLOG_TRACE(R"(Sent {} bytes to "{}")", request.size(), label_);
    return {};
}

// ---------------------------------------------------------------------------
//  read_response  —  reads and parses the HTTP response via picohttpparser
//
//  Supports both Content-Length and Transfer-Encoding: chunked bodies.
//  Returns std::expected for all errors — I/O and parse errors are expected
//  conditions.  Does NOT throw.
// ---------------------------------------------------------------------------

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> DohResolver::Impl::read_response(int cancel_fd) const {
    constexpr size_t INITIAL_BUF_SIZE = 4096;
    constexpr size_t MAX_HEADER_SIZE = 8192;
    constexpr size_t MAX_BODY_SIZE = 65536;

    std::vector<char> buf(INITIAL_BUF_SIZE);
    size_t total_read = 0;

    for (;;) {
        // Attempt to parse the HTTP response so far.
        auto result = Utils::Http::parse_response(std::string_view(buf.data(), total_read), DOH_CONTENT_TYPE,
                                                  MAX_BODY_SIZE);

        if (result) {
            const auto &info = *result;

            // Check HTTP status code.
            if (info.status_code != 200) {
                const auto ec = info.status_code >= 500 ? DnsError::RETRY : DnsError::SERVER_REFUSED;
                persistent_conn_->close();
                return std::unexpected(DnsErrorInfo{
                    ec,
                    fmt::format(R"(Server "{}" returned HTTP status {})", label_, info.status_code)
                });
            }

            const size_t body_buffered = total_read - info.header_end;

            // ── Fixed-length body (Content-Length) ──
            if (info.has_content_length) {
                std::vector<std::uint8_t> body;
                body.reserve(info.content_length);

                if (body_buffered > 0) {
                    const auto *src = reinterpret_cast<const std::uint8_t *>(buf.data() + info.header_end);
                    body.insert(body.end(), src, src + body_buffered);
                }

                if (body_buffered < info.content_length) {
                    body.resize(info.content_length);
                    auto *dst = body.data() + body_buffered;
                    const auto remaining = info.content_length - body_buffered;
                    auto read_status = persistent_conn_->read_exact(std::span(dst, remaining), cancel_fd);

                    if (!read_status) {
                        if (read_status.error() == TlsConnection::IoStatus::CANCELLED) {
                            persistent_conn_->close();
                            return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "Query cancelled"});
                        }
                        persistent_conn_->close();
                        return std::unexpected(DnsErrorInfo{
                            DnsError::CONNECTION,
                            fmt::format(R"(Failed to read response body from "{}")", label_)
                        });
                    }
                }

                return body;
            }

            // ── Chunked transfer encoding ──
            // Parse the buffered body data first, then read from the network.
            std::vector<std::uint8_t> body;
            body.reserve(MAX_BODY_SIZE);

            // Wraps buffered data + network read into a single logical stream.
            size_t buf_used = 0;

            auto stream_read = [&](std::span<std::uint8_t> dst) -> std::expected<void, DnsErrorInfo> {
                size_t need = dst.size();
                while (need > 0) {
                    // Consume from buffer first.
                    if (buf_used < body_buffered) {
                        const size_t avail = body_buffered - buf_used;
                        const size_t take = std::min(need, avail);
                        const auto *src = reinterpret_cast<const std::uint8_t *>(buf.data() + info.header_end + buf_used);
                        std::ranges::copy_n(src, static_cast<std::ptrdiff_t>(take), dst.data());
                        buf_used += take;
                        dst = dst.subspan(take);
                        need -= take;
                    } else {
                        auto status = persistent_conn_->read_exact(dst, cancel_fd);
                        if (!status) {
                            if (status.error() == TlsConnection::IoStatus::CANCELLED) {
                                return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "Query cancelled"});
                            }
                            persistent_conn_->close();
                            return std::unexpected(DnsErrorInfo{
                                DnsError::CONNECTION,
                                fmt::format(R"(Failed to read chunked body from "{}")", label_)
                            });
                        }
                        need = 0;
                    }
                }
                return {};
            };

            // Read a single byte.
            auto read_byte = [&]() -> std::expected<std::uint8_t, DnsErrorInfo> {
                std::uint8_t b;
                auto r = stream_read(std::span(&b, 1));
                if (!r) return std::unexpected(std::move(r.error()));
                return b;
            };

            // Read a CRLF-terminated line (without the trailing CRLF).
            auto read_line = [&](std::string &line) -> std::expected<void, DnsErrorInfo> {
                line.clear();
                bool got_cr = false;
                for (;;) {
                    auto b = read_byte();
                    if (!b) return std::unexpected(std::move(b.error()));
                    if (*b == '\n' && got_cr) break;
                    if (got_cr) {
                        line += '\r';
                        got_cr = false;
                    }
                    if (*b == '\r') { got_cr = true; continue; }
                    line += static_cast<char>(*b);
                }
                return {};
            };

            // Main chunked decoding loop.
            size_t body_size = 0;
            for (;;) {
                std::string line;
                auto r = read_line(line);
                if (!r) return std::unexpected(std::move(r.error()));

                // Parse hex chunk size (ignore chunk-ext after ';').
                const auto semi = line.find(';');
                const auto size_str = (semi != std::string::npos) ? line.substr(0, semi) : line;
                size_t chunk_size = 0;
                try {
                    chunk_size = std::stoul(size_str, nullptr, 16);
                } catch (const std::exception &) {
                    persistent_conn_->close();
                    return std::unexpected(DnsErrorInfo{
                        DnsError::PARSE,
                        fmt::format(R"(Server "{}" returned invalid chunk size)", label_)
                    });
                }

                // Zero-length chunk marks the end.
                if (chunk_size == 0) {
                    break;
                }

                // Enforce maximum body size.
                if (body_size + chunk_size > MAX_BODY_SIZE) {
                    persistent_conn_->close();
                    return std::unexpected(DnsErrorInfo{
                        DnsError::PARSE,
                        fmt::format(R"(Chunked response from "{}" exceeds maximum size)", label_)
                    });
                }

                // Read chunk data.
                body.resize(body_size + chunk_size);
                auto cr = stream_read(std::span(body.data() + body_size, chunk_size));
                if (!cr) return std::unexpected(std::move(cr.error()));
                body_size += chunk_size;

                // Read trailing CRLF.
                std::uint8_t crlf[2];
                auto cr2 = stream_read(crlf);
                if (!cr2) return std::unexpected(std::move(cr2.error()));
            }

            // Read the trailing CRLF after the last chunk (empty trailer).
            std::uint8_t crlf[2];
            auto cr = stream_read(crlf);
            if (!cr) return std::unexpected(std::move(cr.error()));

            body.resize(body_size);
            return body;
        }

        // Parse failed or needs more data — distinguish by error type.
        if (result.error() != Utils::Http::HttpError::INCOMPLETE) {
            persistent_conn_->close();
            return std::unexpected(DnsErrorInfo{
                DnsError::PARSE,
                fmt::format(R"(Server "{}" returned malformed HTTP response)", label_)
            });
        }

        // Need more data — check size limit, grow buffer, and read more.
        if (total_read >= MAX_HEADER_SIZE) {
            persistent_conn_->close();
            return std::unexpected(DnsErrorInfo{
                DnsError::PARSE,
                fmt::format(R"(Server "{}" response headers exceed maximum size)", label_)
            });
        }

        if (total_read == buf.size()) {
            buf.resize(buf.size() * 2);
        }

        auto *read_ptr = reinterpret_cast<std::uint8_t *>(buf.data() + total_read);
        const auto read_capacity = buf.size() - total_read;

        auto read_result = persistent_conn_->read_some(std::span<std::uint8_t>(read_ptr, read_capacity), cancel_fd);

        if (!read_result) {
            if (read_result.error() == TlsConnection::IoStatus::CANCELLED) {
                persistent_conn_->close();
                return std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "Query cancelled"});
            }
            persistent_conn_->close();
            return std::unexpected(DnsErrorInfo{
                DnsError::CONNECTION,
                fmt::format(R"(Failed to read response from "{}")", label_)
            });
        }

        total_read += *read_result;
    }
}

// ---------------------------------------------------------------------------
//  ensure_connection  —  manage connection reuse with idle timeout
//
//  Returns std::expected<void, DnsErrorInfo> — empty on success, error on failure.
// ---------------------------------------------------------------------------

std::expected<void, DnsErrorInfo> DohResolver::Impl::ensure_connection() const {
    const auto now = std::chrono::steady_clock::now();

    if (persistent_conn_ && persistent_conn_->is_connected()) {
        const auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_use_);
        if (idle < IDLE_TIMEOUT) [[likely]] {
            if (persistent_conn_->is_healthy()) [[likely]] {
                return {};
            }
            SPDLOG_TRACE(R"(Server closed connection to "{}", reconnecting)", label_);
            persistent_conn_->close();
        } else {
            SPDLOG_TRACE(R"(Idle timeout ({}s) for "{}", reconnecting)", idle.count(), label_);
            persistent_conn_->close();
        }
    }

    if (!persistent_conn_) {
        persistent_conn_ = std::make_unique<TlsConnection>(
            server_, port_, CONNECT_TIMEOUT, std::nullopt, std::span<const unsigned char>(ALPN_HTTP)
        );
    }

    auto result = persistent_conn_->connect();
    if (!result) {
        if (result.error() == TlsConnection::IoStatus::TIMEOUT) {
            return std::unexpected(DnsErrorInfo{DnsError::RETRY,
                fmt::format(R"(Connection to "{}" timed out)", label_)});
        }
        return std::unexpected(DnsErrorInfo{DnsError::CONNECTION,
            fmt::format(R"(Connection to "{}" failed)", label_)});
    }

    last_use_ = now;
    return {};
}

// ===========================================================================
//  DohResolver  —  public API
// ===========================================================================

DohResolver::DohResolver(std::string host, std::uint16_t port, std::string path, std::string label)
    : impl_(std::make_unique<Impl>(std::move(host), port, std::move(path), get_id(), std::move(label))) {
}

DohResolver::~DohResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
DohResolver::query(const std::string &host, RecordKind type, int cancel_fd) const {
    return impl_->query(host, type, cancel_fd);
}

// ===========================================================================
//  Self-registration
// ===========================================================================

namespace {
    [[maybe_unused]] DnsResolverRegistry::Registrar _doh(
        "https",
        [](const Config::DnsServer &server) -> std::unique_ptr<ResolverBase> {
            auto uri = Uri::parse(server.address);
            auto host = std::string(uri.get_host());
            auto port = static_cast<std::uint16_t>(uri.get_port() != 0 ? uri.get_port() : 443);
            auto path = std::string(uri.get_path());
            if (path.empty()) {
                path = "/";
            }
            return std::make_unique<DohResolver>(std::move(host), port, std::move(path), std::string(uri.get_origin()));
        });
} // namespace
