//
// Component tests for net::http::Client and net::http::Session.
//
// Drives the full stack over an in-process loopback HTTP/1.1 server
// (thread-per-connection, keep-alive aware): GET/POST roundtrips, query
// strings, redirects, redirect loops, size limits, refused connections,
// invalid URLs, and cross-thread cancellation mid-exchange.
// =============================================================================

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http_client/client.h"
#include "http_client/persistent_client.h"
#include "http_client/stream_factory.h"
#include "http_client/session.h"
#include "util/cancellation_token.hpp"

using namespace std::chrono_literals;
using net::http::ErrorCode;
using net::http::Method;

namespace {

// ===========================================================================
//  Minimal in-process HTTP/1.1 test server (thread-per-connection).
// ===========================================================================

struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
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
            // Wake a blocked accept() with a dummy self-connection (closing
            // the fd alone does not reliably interrupt accept on Linux).
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

    /// Base URL for http:// origins pointing at this server.
    [[nodiscard]] std::string base_url() const { return fmt_url(port_); }

    /// Total accepted connections since start (keep-alive assertions).
    [[nodiscard]] int connection_count() const noexcept {
        return connections_.load(std::memory_order_relaxed);
    }

    static std::string fmt_url(const std::uint16_t port) { return "http://127.0.0.1:" + std::to_string(port); }

private:
    [[nodiscard]] static std::string response(const int status,
                                              std::string reason,
                                              std::vector<std::pair<std::string, std::string>> headers,
                                              const std::string& body,
                                              const std::string_view version = "HTTP/1.1") {
        headers.emplace_back("Content-Length", std::to_string(body.size()));
        std::string out = fmt_line(status, std::move(reason), version);
        for (const auto& [k, v] : headers) {
            out += k + ": " + v + "\r\n";
        }
        out += "\r\n";
        out += body;
        return out;
    }

    [[nodiscard]] static std::string fmt_line(const int status, std::string reason,
                                              const std::string_view version = "HTTP/1.1") {
        return std::string(version) + " " + std::to_string(status) + " " + std::move(reason) + "\r\n";
    }

    [[nodiscard]] static std::string chunked_response(const std::vector<std::string>& chunks) {
        std::string out = fmt_line(200, "OK");
        out += "Transfer-Encoding: chunked\r\n\r\n";
        for (const auto& c : chunks) {
            char hex[16];
            const auto n = std::snprintf(hex, sizeof(hex), "%zx", c.size());
            out.append(hex, static_cast<size_t>(n));
            out += "\r\n";
            out += c;
            out += "\r\n";
        }
        out += "0\r\n\r\n";
        return out;
    }

    [[nodiscard]] std::string route(const HttpRequest& req) const {
        if (req.method == "GET" && req.target == "/hello") {
            return response(200, "OK", {{"Content-Type", "text/plain"}}, "hello world");
        }
        if (req.method == "GET" && req.target.starts_with("/query")) {
            return response(200, "OK", {}, "target=" + req.target);
        }
        if (req.method == "POST" && req.target == "/echo") {
            return response(200, "OK", {{"Content-Type", "application/octet-stream"}}, req.body);
        }
        if (req.method == "GET" && req.target == "/chunked") {
            return chunked_response({"part1-", "part2-", "part3"});
        }
        if (req.method == "GET" && req.target == "/http10-keep-alive") {
            if (req.version != "HTTP/1.0" || !req.headers.contains("connection") ||
                req.headers.at("connection") != "keep-alive") {
                return response(400, "Bad Request", {}, "missing HTTP/1.0 keep-alive");
            }
            return response(200, "OK", {{"Connection", "Keep-Alive"}}, "legacy", "HTTP/1.0");
        }
        if (req.method == "GET" && req.target == "/redirect") {
            return response(302, "Found", {{"Location", "/hello"}}, "");
        }
        if (req.method == "GET" && req.target == "/redirect-loop") {
            return response(302, "Found", {{"Location", "/redirect-loop"}}, "");
        }
        if (req.method == "GET" && req.target == "/slow") {
            // Headers immediately, body after a long delay — exercises
            // cancellation while blocked mid-exchange.
            return fmt_line(200, "OK") + "Content-Length: 4\r\n\r\n";
        }
        if (req.method == "GET" && req.target.starts_with("/big")) {
            return response(200, "OK", {}, std::string(4096, 'x'));
        }
        return response(404, "Not Found", {}, "no such route");
    }

    void handle_connection(const int conn) const {
        std::string pending;
        std::array<char, 4096> buf{};
        for (;;) {
            // Read until the full header block is present.
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

            // Ensure the full body has arrived.
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
        req.version = first_line.substr(sp2 + 1);

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
            connections_.fetch_add(1, std::memory_order_relaxed);
            std::jthread([this, conn] { handle_connection(conn); }).detach();
        }
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    bool running_ = false;
    std::jthread accept_thread_;
    std::atomic<int> connections_{0};
};

// ===========================================================================
//  Test fixture
// ===========================================================================

class HttpClientTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { server_.start(); }

    static void TearDownTestSuite() { server_.stop(); }

    [[nodiscard]] static net::http::Client make_client() {
        net::http::Options opts;
        opts.user_agent = "net-http-test/1.0";
        return net::http::Client(std::move(opts));
    }

    inline static HttpTestServer server_;
};

// ===========================================================================
//  Client
// ===========================================================================

TEST_F(HttpClientTest, Get_ReturnsStatusHeadersBody) {
    auto client = make_client();
    net::http::Request req{.method = Method::GET};

    auto resp = client.exchange(server_.base_url() + "/hello", req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "hello world");
    EXPECT_EQ(resp->headers.count("Content-Type"), 1);
}

TEST_F(HttpClientTest, Get_QueryStringPreserved) {
    auto client = make_client();
    net::http::Request req{.method = Method::GET};

    auto resp = client.exchange(server_.base_url() + "/query?a=1&b=two", req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->text(), "target=/query?a=1&b=two");
}

TEST_F(HttpClientTest, Post_EchoesBody) {
    auto client = make_client();
    net::http::Request req{
        .method = Method::POST,
        .headers = {{"X-Custom", "yes"}},
        .body = std::string("payload-123"),
        .content_type = "text/plain",
    };

    auto resp = client.exchange(server_.base_url() + "/echo", req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "payload-123");
}

TEST_F(HttpClientTest, ChunkedResponse_Assembled) {
    auto client = make_client();
    net::http::Request req{.method = Method::GET};

    auto resp = client.exchange(server_.base_url() + "/chunked", req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->text(), "part1-part2-part3");
}

TEST_F(HttpClientTest, Redirect_IsFollowed) {
    auto client = make_client();
    net::http::Request req{.method = Method::GET};

    auto resp = client.exchange(server_.base_url() + "/redirect", req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "hello world");
}

TEST_F(HttpClientTest, RedirectLoop_LimitExceeded) {
    auto client = make_client();
    net::http::Request req{.method = Method::GET};

    auto resp = client.exchange(server_.base_url() + "/redirect-loop", req);
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::REDIRECT_LIMIT_EXCEEDED);
}

TEST_F(HttpClientTest, BodyLimit_Enforced) {
    net::http::Options opts;
    opts.limits.max_body_bytes = 100;
    net::http::Client client(std::move(opts));

    net::http::Request req{.method = Method::GET};
    auto resp = client.exchange(server_.base_url() + "/big", req);
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::BODY_TOO_LARGE);
}

TEST_F(HttpClientTest, ConnectionRefused_ConnectFailed) {
    auto client = make_client();
    net::http::Request req{.method = Method::GET};

    auto resp = client.exchange(HttpTestServer::fmt_url(1), req);  // nothing listens
    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::CONNECT_FAILED);
}

TEST_F(HttpClientTest, InvalidUrl_Rejected) {
    auto client = make_client();
    net::http::Request req{.method = Method::GET};

    EXPECT_EQ(client.exchange("ftp://example.com/x", req).error().code, ErrorCode::INVALID_URL);
    EXPECT_EQ(client.exchange("http://", req).error().code, ErrorCode::INVALID_URL);
}

TEST_F(HttpClientTest, CancelMidExchange_ReturnsCancelled) {
    Utils::CancellationSource source;
    net::http::Options opts;
    opts.transport.read_timeout = 30s;
    net::http::Client client(std::move(opts), source.token());

    net::http::Request req{.method = Method::GET};
    std::jthread triggerrer([src = source] {
        std::this_thread::sleep_for(100ms);
        src.trigger();
    });

    const auto start = std::chrono::steady_clock::now();
    auto resp = client.exchange(server_.base_url() + "/slow", req);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::CANCELLED);
    EXPECT_LT(elapsed, 5s);
}

// ===========================================================================
//  PersistentClient (base URL + connection reuse)
// ===========================================================================

TEST_F(HttpClientTest, PersistentClient_ExchangesAgainstBaseOrigin) {
    net::http::PersistentClient client(server_.base_url());
    net::http::Request req{.method = Method::GET};

    auto resp = client.exchange("/hello", req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "hello world");
}

TEST_F(HttpClientTest, PersistentClient_ReusesConnectionAcrossExchanges) {
    net::http::PersistentClient client(server_.base_url());
    net::http::Request req{.method = Method::GET};

    const auto before = server_.connection_count();
    ASSERT_TRUE(client.exchange("/hello", req));
    ASSERT_TRUE(client.exchange("/query?x=1", req));

    // Both exchanges ran on ONE connection (keep-alive).
    EXPECT_EQ(server_.connection_count(), before + 1);
}

TEST_F(HttpClientTest, PersistentClient_Http10KeepAlive_ReusesConnection) {
    net::http::Options opts{.version = net::http::HttpVersion::V1_0};
    net::http::PersistentClient client(server_.base_url(), opts);
    net::http::Request req{.method = Method::GET};

    const auto before = server_.connection_count();
    auto first = client.exchange("/http10-keep-alive", req);
    auto second = client.exchange("/http10-keep-alive", req);

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->text(), "legacy");
    EXPECT_EQ(second->text(), "legacy");
    EXPECT_EQ(server_.connection_count(), before + 1);
}

TEST_F(HttpClientTest, PersistentClient_FollowsSameOriginRedirect) {
    net::http::PersistentClient client(server_.base_url());
    net::http::Request req{.method = Method::GET};

    auto resp = client.exchange("/redirect", req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "hello world");
}

TEST_F(HttpClientTest, PersistentClient_InvalidBaseUrl_Throws) {
    EXPECT_THROW((net::http::PersistentClient("ftp://example.com")), std::invalid_argument);
    EXPECT_THROW((net::http::PersistentClient("http://")), std::invalid_argument);
}

// ===========================================================================
//  Session (persistent reuse)
// ===========================================================================

TEST_F(HttpClientTest, Session_ReusesConnectionAcrossExchanges) {
    net::http::Session session(std::make_shared<net::http::DefaultStreamFactory>(), {}, {}, "http", "127.0.0.1",
                               server_.port(), {});

    // The server handles sequential requests on one keep-alive connection.
    net::http::protocol::WireRequest first{
        .method = Method::GET, .target = "/hello", .headers = {{"Host", "127.0.0.1"}}};
    auto r1 = session.exchange(first);
    ASSERT_TRUE(r1);
    EXPECT_EQ(r1->text(), "hello world");

    net::http::protocol::WireRequest second{
        .method = Method::GET, .target = "/query?x=1", .headers = {{"Host", "127.0.0.1"}}};
    auto r2 = session.exchange(second);
    ASSERT_TRUE(r2);
    EXPECT_EQ(r2->text(), "target=/query?x=1");
}

}  // namespace
