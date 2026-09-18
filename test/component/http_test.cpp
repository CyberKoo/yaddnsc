//
// Component tests for the net::http-backed HttpIpSource and the production
// client wiring, over an in-process loopback HTTP server.
//
// Coverage includes cross-thread cancellation mid-exchange: the /hang route
// acknowledges the request to the test and then holds the response until the
// test releases it, so a triggered token — not a timeout — must unblock the
// client. One test drives the whole UpdateWorkflow through that path (real
// IpSourceAdapter + real HttpIpSource, mock DNS/driver ports): the workflow's
// CANCELLED branch must be reachable from the real HTTP source, not only from
// a mock port returning CANCELLED.
// =============================================================================

#include "infrastructure/ip_source/http.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <expected>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "application/update_workflow.h"
#include "domain/config/ip_source_kind.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/fqdn.h"
#include "domain/network/inet_address.h"
#include "domain/update/update_task.h"
#include "infrastructure/ip_source/adapter.h"
#include "infrastructure/network/http/client.h"
#include "infrastructure/network/http/types.h"
#include "mocks/mock_ports.h"
#include "mocks/null_logger.h"
#include "support/util/cancellation_token.hpp"

using namespace std::chrono_literals;
using ::testing::_;

namespace {

// ===========================================================================
//  Minimal in-process HTTP/1.1 test server (thread-per-connection).
// ===========================================================================

struct HttpRequest {
    std::string method;
    std::string target;
    std::map<std::string, std::string> headers;  // lowercased names
    std::string body;
};

class HttpTestServer {
public:
    void start() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(listener_, 0);

        int one = 1;
        ASSERT_EQ(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        ASSERT_EQ(::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        ASSERT_EQ(::listen(listener_, 8), 0);

        socklen_t len = sizeof(addr);
        ASSERT_EQ(::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        port_ = ntohs(addr.sin_port);

        running_ = true;
        accept_thread_ = std::jthread([this] { accept_loop(); });
    }

    void stop() {
        if (!running_) {
            return;
        }
        running_ = false;
        if (listener_ >= 0) {
            const int wake = ::socket(AF_INET, SOCK_STREAM, 0);
            if (wake >= 0) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                addr.sin_port = htons(port_);
                ::connect(wake, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
                ::close(wake);
            }
            ::close(listener_);
            listener_ = -1;
        }
    }

    ~HttpTestServer() { stop(); }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    /// Arm the /hang route: the first request to it fulfils `entered` and the
    /// handler parks without responding until release_hang() — the client is
    /// expected to unblock through cancellation, not a timeout.
    void arm_hang(std::shared_ptr<std::promise<void>> entered) { hang_entered_ = std::move(entered); }

    /// Let a parked /hang handler close its connection and exit.
    void release_hang() {
        {
            std::lock_guard lock(hang_mtx_);
            hang_release_ = true;
        }
        hang_cv_.notify_all();
    }

private:
    [[nodiscard]] static std::string route(const HttpRequest& req) {
        if (req.method == "GET" && req.target == "/ip") {
            return response(200, "203.0.113.7");
        }
        if (req.method == "GET" && req.target == "/bad") {
            return response(200, "not-an-ip-address");
        }
        if (req.method == "POST" && req.target == "/echo") {
            return response(200, req.body);
        }
        return response(404, "no such route");
    }

    [[nodiscard]] static std::string response(const int status, const std::string& body) {
        std::string out = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK\r\n" : " Not Found\r\n");
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        out += body;
        return out;
    }

    void handle_connection(const int conn) const {
        std::string pending;
        std::array<char, 4096> buf{};
        for (;;) {
            auto header_end = pending.find("\r\n\r\n");
            while (header_end == std::string::npos) {
                const ssize_t n = ::recv(conn, buf.data(), buf.size(), 0);
                if (n <= 0) {
                    ::close(conn);
                    return;
                }
                pending.append(buf.data(), static_cast<size_t>(n));
                header_end = pending.find("\r\n\r\n");
            }

            HttpRequest req = parse_request(pending.substr(0, header_end));
            const size_t content_length =
                req.headers.contains("content-length") ? std::stoull(req.headers["content-length"]) : 0;
            const size_t need = header_end + 4 + content_length;
            while (pending.size() < need) {
                const ssize_t n = ::recv(conn, buf.data(), buf.size(), 0);
                if (n <= 0) {
                    ::close(conn);
                    return;
                }
                pending.append(buf.data(), static_cast<size_t>(n));
            }
            req.body = pending.substr(header_end + 4, content_length);
            pending.erase(0, need);

            if (req.method == "GET" && req.target == "/hang") {
                // The request reached the server: the exchange is in flight.
                // Hold the response until the test releases it (or the process
                // ends) so the client must be unblocked by cancellation.
                if (hang_entered_) {
                    hang_entered_->set_value();
                }
                std::unique_lock lock(hang_mtx_);
                hang_cv_.wait(lock, [this] { return hang_release_; });
                ::close(conn);
                return;
            }

            const std::string out = route(req);
            ssize_t sent = 0;
            while (sent < static_cast<ssize_t>(out.size())) {
                const ssize_t n = ::send(conn, out.data() + sent, out.size() - static_cast<size_t>(sent), MSG_NOSIGNAL);
                if (n <= 0) {
                    ::close(conn);
                    return;
                }
                sent += n;
            }
        }
    }

    [[nodiscard]] static HttpRequest parse_request(const std::string& header_block) {
        HttpRequest req;
        const auto first_line_end = header_block.find("\r\n");
        const auto first_line = header_block.substr(0, first_line_end);

        const auto sp1 = first_line.find(' ');
        const auto sp2 = first_line.find(' ', sp1 + 1);
        req.method = first_line.substr(0, sp1);
        req.target = first_line.substr(sp1 + 1, sp2 - sp1 - 1);

        size_t pos = first_line_end + 2;
        while (pos < header_block.size()) {
            const auto line_end = header_block.find("\r\n", pos);
            if (line_end == std::string::npos || line_end == pos) {
                break;
            }
            const auto colon = header_block.find(':', pos);
            auto name = header_block.substr(pos, colon - pos);
            auto value = header_block.substr(colon + 1, line_end - colon - 1);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            while (!value.empty() && value.front() == ' ') {
                value.erase(value.begin());
            }
            req.headers[std::move(name)] = value;
            pos = line_end + 2;
        }
        return req;
    }

    void accept_loop() {
        while (running_) {
            const int conn = ::accept(listener_, nullptr, nullptr);
            if (conn < 0) {
                break;
            }
            std::jthread([this, conn] { handle_connection(conn); }).detach();
        }
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    bool running_ = false;
    std::jthread accept_thread_;
    // Synchronisation state for /hang; handle_connection() is const, and
    // mutable is the idiomatic escape hatch for mutex-guarded state.
    mutable std::shared_ptr<std::promise<void>> hang_entered_;
    mutable std::mutex hang_mtx_;
    mutable std::condition_variable hang_cv_;
    mutable bool hang_release_ = false;
};

// ===========================================================================
//  Test fixture
// ===========================================================================

class HttpFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() { server_.start(); }

    static void TearDownTestSuite() { server_.stop(); }

    inline static HttpTestServer server_;
};

}  // namespace

// ── HttpIpSource ─────────────────────────────────────────────────────────────

TEST_F(HttpFixture, HttpIpSource_ResolvesIpFromBody) {
    const HttpIpSource source(server_.base_url() + "/ip");
    const auto result = source.resolve({});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->front().to_string(), "203.0.113.7");
}

TEST_F(HttpFixture, HttpIpSource_UnparseableBodyReturnsUnavailable) {
    const HttpIpSource source(server_.base_url() + "/bad");
    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::UNAVAILABLE);
}

TEST_F(HttpFixture, HttpIpSource_ConnectionRefusedReturnsUnavailable) {
    const HttpIpSource source("http://127.0.0.1:1/ip");  // nothing listens
    const auto result = source.resolve({});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::UNAVAILABLE);
}

TEST_F(HttpFixture, HttpIpSource_InitiallyCancelledReturnsCancelled) {
    const HttpIpSource source(server_.base_url() + "/ip");
    Utils::CancellationSource cancellation;
    cancellation.trigger();

    const auto result = source.resolve(cancellation.token());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::IpSourceError::Code::CANCELLED);
}

// Mid-exchange cancellation: the server holds the response, the token fires
// while the client waits — the source must map the transport cancellation to
// IpSourceError::CANCELLED (plan 4.5), not wait out the read timeout.
TEST_F(HttpFixture, HttpIpSource_CancelMidExchangeReturnsCancelled) {
    auto entered = std::make_shared<std::promise<void>>();
    auto seen = entered->get_future();
    server_.arm_hang(std::move(entered));

    const HttpIpSource source(server_.base_url() + "/hang");
    Utils::CancellationSource cancellation;

    std::optional<IpSourceBase::Result> result;
    std::jthread worker([&] { result = source.resolve(cancellation.token()); });

    ASSERT_EQ(seen.wait_for(10s), std::future_status::ready) << "the HTTP exchange never started";
    cancellation.trigger();

    const auto start = std::chrono::steady_clock::now();
    worker.join();
    server_.release_hang();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, domain::IpSourceError::Code::CANCELLED);
    EXPECT_LT(elapsed, 5s);
}

// ── Production client (transient, through the connection factory) ────────────

TEST_F(HttpFixture, Client_GetRoundtrip) {
    net::http::Client client({});
    net::http::Request req{.method = net::http::Method::GET};

    auto resp = client.exchange(server_.base_url() + "/ip", req, {});
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "203.0.113.7");
}

TEST_F(HttpFixture, Client_PostEchoesBinaryBody) {
    net::http::Client client({});
    const std::string payload{'a', '\0', 'b', '\0', 'c'};  // binary-safe
    net::http::Request req{.method = net::http::Method::POST};
    req.set_body(std::span(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
    req.content_type = "application/octet-stream";

    auto resp = client.exchange(server_.base_url() + "/echo", req, {});
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    const auto echoed = resp->bytes();
    ASSERT_EQ(echoed.size(), payload.size());
    EXPECT_TRUE(std::equal(echoed.begin(), echoed.end(), payload.begin()));
}

// ── UpdateWorkflow over the real HTTP source ─────────────────────────────────
// Plan 4.4: IpSourceError::CANCELLED → UpdateError::CANCELLED must be
// reachable through the real HTTP path (adapter + factory + HttpIpSource
// against a holding server), not only through a mock port returning CANCELLED.

namespace {

// Single-subdomain runtime config whose HTTP source points at `url`.
[[nodiscard]] std::shared_ptr<const domain::RuntimeConfig> http_source_config(const std::string& url) {
    domain::RuntimeConfig config;
    config.domains.push_back(domain::DomainConfig{
        .name = "example.com",
        .update_interval = 300,
        .force_update = 0,
        .driver = "cloudflare",
        .subdomains = {domain::SubdomainConfig{
            .name = "www",
            .type = RecordKind::A,
            .ip_source = Config::IpSource::HTTP,
            .ip_source_param = url,
            .update_interval = 300,
        }},
    });
    return std::make_shared<const domain::RuntimeConfig>(std::move(config));
}

[[nodiscard]] domain::UpdateTask make_task(const std::shared_ptr<const domain::RuntimeConfig>& cfg) {
    const auto& domain_cfg = cfg->domains[0];
    const auto& sub = domain_cfg.subdomains[0];
    return domain::UpdateTask{
        .config = cfg,
        .domain_index = 0,
        .subdomain_index = 0,
        .fqdn = domain::make_fqdn(domain_cfg.name, sub.name),
        .force_update = false,
    };
}

}  // namespace

TEST_F(HttpFixture, UpdateWorkflow_CancelMidHttpExchangeReturnsCancelled) {
    auto entered = std::make_shared<std::promise<void>>();
    auto seen = entered->get_future();
    server_.arm_hang(std::move(entered));

    const auto cfg = http_source_config(server_.base_url() + "/hang");
    const auto task = make_task(cfg);

    MockDnsResolverPort dns;
    MockDriverGateway gateway;
    NullLogger logger;
    IpSourceAdapter ip_source;  // default factory: SubdomainConfig → HttpIpSource
    const UpdateWorkflow workflow(dns, ip_source, gateway, logger);

    // A cancelled IP source lookup must stop the cycle before the DNS read
    // and before any driver call.
    EXPECT_CALL(dns, resolve(_, _, _)).Times(0);
    EXPECT_CALL(gateway, update(_, _, _)).Times(0);

    Utils::CancellationSource cancellation;
    std::optional<UpdateOutcome> outcome;
    std::jthread worker([&] { outcome = workflow.run(task, cancellation.token()); });

    ASSERT_EQ(seen.wait_for(10s), std::future_status::ready) << "the HTTP exchange never started";
    cancellation.trigger();
    worker.join();
    server_.release_hang();

    ASSERT_TRUE(outcome.has_value());
    ASSERT_FALSE(outcome->has_value());
    EXPECT_EQ(outcome->error().code, domain::UpdateError::Code::CANCELLED);
}
