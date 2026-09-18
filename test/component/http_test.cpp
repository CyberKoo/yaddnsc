//
// Component tests for the net::http-backed HttpIpSource and the production
// client wiring, over an in-process loopback HTTP server.
// =============================================================================

#include "infrastructure/ip_source/http.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
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

#include "domain/network/inet_address.h"
#include "infrastructure/network/http/client.h"
#include "infrastructure/network/http/types.h"

using namespace std::chrono_literals;

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
    const auto addresses = source.resolve();
    ASSERT_EQ(addresses.size(), 1);
    EXPECT_EQ(addresses.front().to_string(), "203.0.113.7");
}

TEST_F(HttpFixture, HttpIpSource_ThrowsOnUnparseableBody) {
    const HttpIpSource source(server_.base_url() + "/bad");
    EXPECT_THROW(std::ignore = source.resolve(), std::runtime_error);
}

TEST_F(HttpFixture, HttpIpSource_ThrowsOnConnectionRefused) {
    const HttpIpSource source("http://127.0.0.1:1/ip");  // nothing listens
    EXPECT_THROW(std::ignore = source.resolve(), std::runtime_error);
}

// ── Production client (transient, through the connection factory) ────────────

TEST_F(HttpFixture, Client_GetRoundtrip) {
    net::http::Client client({});
    net::http::Request req{.method = net::http::Method::GET};

    auto resp = client.exchange(server_.base_url() + "/ip", req);
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

    auto resp = client.exchange(server_.base_url() + "/echo", req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    const auto echoed = resp->bytes();
    ASSERT_EQ(echoed.size(), payload.size());
    EXPECT_TRUE(std::equal(echoed.begin(), echoed.end(), payload.begin()));
}
