//
// Integration tests for http_client and HttpIpSource using a local
// cpp-httplib server on the loopback interface.
//
// No external network required — the server runs in-process on 127.0.0.1.
//
// =============================================================================

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <net/if.h>
#include <string>
#include <thread>

#include <httplib.h>
#include <gtest/gtest.h>

#include "ip_source/http.h"
#include "network/http_client.h"
#include "network/inet_address.h"
#include "uri.h"

#include "fmt.hpp"

using namespace std::chrono_literals;

// ===========================================================================
// Helpers
// ===========================================================================

namespace {
    /// Find the loopback interface name at runtime.
    /// Linux uses "lo", macOS/BSD uses "lo0".
    [[nodiscard]] std::string loopback_interface_name() {
        if (::if_nametoindex("lo") != 0) return "lo";
        if (::if_nametoindex("lo0") != 0) return "lo0";
        return {};
    }
} // anonymous namespace

// ===========================================================================
// Fixture: local HTTP server
// ===========================================================================

class HttpServerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<httplib::Server>();

        // Regular IP endpoint.
        server_->Get("/ip", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.set_content("198.51.100.42", "text/plain");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // JSON endpoints.
        server_->Post("/update", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.set_content(R"({"status":"ok"})", "application/json");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        server_->Put("/update", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.set_content(R"({"status":"updated"})", "application/json");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        server_->Delete("/resource", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.status = 204;
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // Non-IP body (error path for HttpIpSource).
        server_->Get("/not-an-ip", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.set_content("this is not an ip address", "text/plain");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // OPTIONS endpoint.
        server_->Options("/options", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.status = 204;
            resp.set_header("Allow", "GET, POST, OPTIONS");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // PATCH endpoint.
        server_->Patch("/patch", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.status = 200;
            resp.set_content(R"({"patched":true})", "application/json");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // Custom-header echo endpoint: returns the value of X-Custom header.
        server_->Get("/custom-headers", [&](const httplib::Request &req, httplib::Response &resp) {
            auto it = req.headers.find("X-Custom");
            if (it != req.headers.end()) {
                resp.set_content(it->second, "text/plain");
            } else {
                resp.status = 400;
                resp.set_content("missing", "text/plain");
            }
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // Bind to a random port on loopback.
        port_ = server_->bind_to_any_port("127.0.0.1");
        ASSERT_GT(port_, 0) << "Failed to bind HTTP server to 127.0.0.1";

        // Start the server listener loop in a background thread.
        server_thread_ = std::thread([this] { server_->listen_after_bind(); });

        // Give the server a moment to start accepting connections.
        std::this_thread::sleep_for(20ms);
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    [[nodiscard]] int port() const { return port_; }
    [[nodiscard]] int hit_count() const { return server_hit_count_.load(); }

private:
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::atomic<int> server_hit_count_{0};
};

// ===========================================================================
// Fixture: local HTTPS server
// ===========================================================================

class HttpsServerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate a self-signed certificate with CA:TRUE.
        char dir_template[] = "/tmp/yaddnsc_https_test_XXXXXX";
        auto *dir = ::mkdtemp(dir_template);
        ASSERT_NE(dir, nullptr) << "mkdtemp failed";

        cert_path_ = std::string(dir) + "/cert.pem";
        key_path_ = std::string(dir) + "/key.pem";

        auto cmd = fmt::format(
            "openssl req -x509 -newkey rsa:2048 -keyout {} -out {} -days 1 -nodes "
            "-subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1 "
            "-addext basicConstraints=critical,CA:TRUE 2>/dev/null",
            key_path_, cert_path_);

        int ret = ::system(cmd.c_str());
        ASSERT_EQ(ret, 0) << "Failed to generate TLS certificate (openssl returned " << ret << ")";

        server_ = std::make_unique<httplib::SSLServer>(cert_path_.c_str(), key_path_.c_str());
        ASSERT_TRUE(server_->is_valid()) << "Failed to create SSLServer";

        server_->Get("/ip", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.set_content("198.51.100.42", "text/plain");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        port_ = server_->bind_to_any_port("127.0.0.1");
        ASSERT_GT(port_, 0) << "Failed to bind HTTPS server to 127.0.0.1";

        server_thread_ = std::thread([this] { server_->listen_after_bind(); });
        std::this_thread::sleep_for(20ms);
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    [[nodiscard]] int port() const { return port_; }
    [[nodiscard]] int hit_count() const { return server_hit_count_.load(); }
    [[nodiscard]] const std::string &cert_path() const { return cert_path_; }

private:
    std::unique_ptr<httplib::SSLServer> server_;
    std::thread server_thread_;
    int port_ = 0;
    std::string cert_path_;
    std::string key_path_;
    std::atomic<int> server_hit_count_{0};
};

// ===========================================================================
// TransientHttpClient — GET
// ===========================================================================

TEST_F(HttpServerFixture, TransientHttpClient_Get) {
    auto before = hit_count();
    TransientHttpClient client;
    auto url = fmt::format("http://127.0.0.1:{}/ip", port());

    HttpRequest req;
    req.method = HttpMethod::GET;

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "HTTP request failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->body, "198.51.100.42");
    EXPECT_EQ(hit_count(), before + 1);
}

TEST_F(HttpServerFixture, TransientHttpClient_Post) {
    auto before = hit_count();
    TransientHttpClient client;
    auto url = fmt::format("http://127.0.0.1:{}/update", port());

    HttpRequest req;
    req.method = HttpMethod::POST;
    req.body = R"({"domain":"example.com"})";
    req.content_type = "application/json";

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "HTTP request failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(hit_count(), before + 1);
}

// ===========================================================================
// TransientHttpClient — error path
// ===========================================================================

TEST_F(HttpServerFixture, TransientHttpClient_ConnectionRefused) {
    TransientHttpClient client;
    // Port 1 — nothing listens there.
    auto url = "http://127.0.0.1:1/nonexistent";

    HttpRequest req;
    req.method = HttpMethod::GET;

    auto result = client.exchange(url, req);
    // Should fail with an error string, not crash.
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().empty());
}

// ===========================================================================
// PersistentHttpClient — GET
// ===========================================================================

TEST_F(HttpServerFixture, PersistentHttpClient_Get) {
    auto before = hit_count();
    // Include the path in the persistent base URI.
    auto uri = Uri::parse(fmt::format("http://127.0.0.1:{}/ip", port()));

    PersistentHttpClient client(uri);

    HttpRequest req;
    req.method = HttpMethod::GET;

    auto result = client.exchange("", req);
    ASSERT_TRUE(result.has_value()) << "HTTP request failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->body, "198.51.100.42");
    EXPECT_EQ(hit_count(), before + 1);
}

// ===========================================================================
// HttpIpSource — resolve IP from the server
// ===========================================================================

TEST_F(HttpServerFixture, HttpIpSource_Resolve) {
    auto before = hit_count();
    auto url = fmt::format("http://127.0.0.1:{}/ip", port());

    HttpIpSource ip_source(url, AddressFamily::UNSPECIFIED);
    auto addrs = ip_source.resolve();

    ASSERT_EQ(addrs.size(), 1U);
    EXPECT_EQ(addrs[0].to_string(), "198.51.100.42");
    EXPECT_EQ(addrs[0].get_family(), AddressFamily::IPV4);
    EXPECT_EQ(hit_count(), before + 1);
}

TEST_F(HttpServerFixture, HttpIpSource_Resolve_UnspecifiedPref) {
    auto url = fmt::format("http://127.0.0.1:{}/ip", port());

    // UNSPECIFIED falls back to AF_UNSPEC which prefers IPv4 on dual-stack hosts.
    HttpIpSource ip_source(url, AddressFamily::UNSPECIFIED);
    auto addrs = ip_source.resolve();

    ASSERT_EQ(addrs.size(), 1U);
    EXPECT_EQ(addrs[0].to_string(), "198.51.100.42");
}

// ===========================================================================
// Additional HTTP methods
// ===========================================================================

TEST_F(HttpServerFixture, TransientHttpClient_Put) {
    auto before = hit_count();
    TransientHttpClient client;
    auto url = fmt::format("http://127.0.0.1:{}/update", port());

    HttpRequest req;
    req.method = HttpMethod::PUT;
    req.body = R"({"key":"value"})";
    req.content_type = "application/json";

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "HTTP PUT failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(hit_count(), before + 1);
}

TEST_F(HttpServerFixture, TransientHttpClient_Delete) {
    auto before = hit_count();
    TransientHttpClient client;
    auto url = fmt::format("http://127.0.0.1:{}/resource", port());

    HttpRequest req;
    req.method = HttpMethod::DEL;

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "HTTP DELETE failed: " << result.error();
    EXPECT_EQ(result->status_code, 204);
    EXPECT_EQ(hit_count(), before + 1);
}

// ===========================================================================
// HttpIpSource — error path (server returns non-IP body)
// ===========================================================================

TEST_F(HttpServerFixture, HttpIpSource_ThrowsOnNonIpBody) {
    auto url = fmt::format("http://127.0.0.1:{}/not-an-ip", port());

    HttpIpSource ip_source(url, AddressFamily::UNSPECIFIED);
    EXPECT_THROW(
        {
            [[maybe_unused]] auto _ = ip_source.resolve();
        },
        std::runtime_error);
}

// ===========================================================================
// Additional HTTP methods — HEAD, OPTIONS, PATCH
// ===========================================================================

TEST_F(HttpServerFixture, TransientHttpClient_Head) {
    auto before = hit_count();
    TransientHttpClient client;
    auto url = fmt::format("http://127.0.0.1:{}/ip", port());

    HttpRequest req;
    req.method = HttpMethod::HEAD;

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "HEAD failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(hit_count(), before + 1);
}

TEST_F(HttpServerFixture, TransientHttpClient_Options) {
    auto before = hit_count();
    TransientHttpClient client;
    auto url = fmt::format("http://127.0.0.1:{}/options", port());

    HttpRequest req;
    req.method = HttpMethod::OPTIONS;

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "OPTIONS failed: " << result.error();
    EXPECT_EQ(result->status_code, 204);
    EXPECT_EQ(hit_count(), before + 1);
}

TEST_F(HttpServerFixture, TransientHttpClient_Patch) {
    auto before = hit_count();
    TransientHttpClient client;
    auto url = fmt::format("http://127.0.0.1:{}/patch", port());

    HttpRequest req;
    req.method = HttpMethod::PATCH;
    req.body = R"({"key":"value"})";
    req.content_type = "application/json";

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "PATCH failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(hit_count(), before + 1);
}

// ===========================================================================
// HttpClient::get_body — convenience helper
// ===========================================================================

TEST_F(HttpServerFixture, TransientHttpClient_GetBody) {
    auto url = fmt::format("http://127.0.0.1:{}/ip", port());

    TransientHttpClient client;
    auto body = client.get_body(url);
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(*body, "198.51.100.42");
}

TEST_F(HttpServerFixture, TransientHttpClient_GetBody_ConnectionRefused) {
    TransientHttpClient client;
    auto body = client.get_body("http://127.0.0.1:1/nonexistent");
    // Connection refused should return std::nullopt.
    EXPECT_FALSE(body.has_value());
}

TEST_F(HttpServerFixture, PersistentHttpClient_GetBody) {
    auto uri = Uri::parse(fmt::format("http://127.0.0.1:{}/ip", port()));
    PersistentHttpClient client(uri);

    auto body = client.get_body("");
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(*body, "198.51.100.42");
}

// ===========================================================================
// TransientHttpClient with custom HttpClientOptions (apply_options branches)
// ===========================================================================

TEST_F(HttpServerFixture, TransientHttpClient_WithCustomOptions) {
    HttpClientOptions opts;
    opts.address_family = AddressFamily::IPV4;
    opts.write_timeout = std::chrono::seconds(3);
    opts.keep_alive = true;
    std::multimap<std::string, std::string> custom_headers = {{"X-Custom", "test-value"}};
    opts.default_headers = std::move(custom_headers);

    auto before = hit_count();
    TransientHttpClient client(opts);
    auto url = fmt::format("http://127.0.0.1:{}/custom-headers", port());

    HttpRequest req;
    req.method = HttpMethod::GET;

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "custom options request failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->body, "test-value");
    EXPECT_EQ(hit_count(), before + 1);
}

TEST_F(HttpServerFixture, TransientHttpClient_InterfaceBinding) {
    // Bind to the loopback interface explicitly.
    auto lo = loopback_interface_name();
    ASSERT_FALSE(lo.empty()) << "no loopback interface found";

    HttpClientOptions opts;
    opts.interface = std::move(lo);

    TransientHttpClient client(opts);
    auto url = fmt::format("http://127.0.0.1:{}/ip", port());

    HttpRequest req;
    req.method = HttpMethod::GET;

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "interface bind request failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->body, "198.51.100.42");
}

// ===========================================================================
// PersistentHttpClient with non-empty URL in exchange
// ===========================================================================

TEST_F(HttpServerFixture, PersistentHttpClient_ExchangeWithUrl) {
    auto base_uri = Uri::parse(fmt::format("http://127.0.0.1:{}", port()));
    PersistentHttpClient client(base_uri);

    HttpRequest req;
    req.method = HttpMethod::GET;

    // Pass a full URL; PersistentHttpClient should extract the path.
    auto result = client.exchange(fmt::format("http://127.0.0.1:{}/ip", port()), req);
    ASSERT_TRUE(result.has_value()) << "Persistent GET failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->body, "198.51.100.42");
}

// ===========================================================================
// Connection timeout — short timeout to an unresponsive port
// ===========================================================================

TEST_F(HttpServerFixture, TransientHttpClient_ConnectionTimeout) {
    HttpClientOptions opts;
    opts.connection_timeout = std::chrono::seconds(1);

    TransientHttpClient client(opts);
    // Port 1 is not open; connection should fail quickly.
    auto url = "http://127.0.0.1:1/nonexistent";

    HttpRequest req;
    req.method = HttpMethod::GET;

    auto result = client.exchange(url, req);
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().empty());
}

// ===========================================================================
// HttpIpSource — with bind interface
// ===========================================================================

TEST_F(HttpServerFixture, HttpIpSource_WithBindInterface) {
    // HttpIpSource with an explicit bind interface.
    auto lo = loopback_interface_name();
    ASSERT_FALSE(lo.empty()) << "no loopback interface found";

    auto url = fmt::format("http://127.0.0.1:{}/ip", port());
    HttpIpSource ip_source(url, AddressFamily::UNSPECIFIED, lo);
    auto addrs = ip_source.resolve();
    ASSERT_EQ(addrs.size(), 1U);
    EXPECT_EQ(addrs[0].to_string(), "198.51.100.42");
}

// ===========================================================================
// HttpIpSource — connection failure (throws on HTTP error)
// ===========================================================================

TEST_F(HttpServerFixture, HttpIpSource_ConnectionFailure_Throws) {
    // Connect to port with nothing listening to trigger HTTP client error.
    HttpIpSource ip_source("http://127.0.0.1:1/nonexistent", AddressFamily::UNSPECIFIED);
    EXPECT_THROW(
        {
            [[maybe_unused]] auto _ = ip_source.resolve();
        },
        std::runtime_error);
}

// ===========================================================================
// TransientHttpClient — HTTPS
// ===========================================================================

TEST_F(HttpsServerFixture, TransientHttpClient_Get_Https) {
    HttpClientOptions opts;
    opts.ca_cert_path = cert_path();
    opts.verify_server_cert = true;
    opts.connection_timeout = std::chrono::seconds(3);

    TransientHttpClient client(opts);
    auto url = fmt::format("https://127.0.0.1:{}/ip", port());

    HttpRequest req;
    req.method = HttpMethod::GET;

    auto result = client.exchange(url, req);
    ASSERT_TRUE(result.has_value()) << "HTTPS request failed: " << result.error();
    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->body, "198.51.100.42");
}
