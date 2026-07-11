//
// Integration tests for http_client and HttpIpSource using a local
// cpp-httplib server on the loopback interface.
//
// No external network required — the server runs in-process on 127.0.0.1.
//
// =============================================================================

#include <atomic>
#include <chrono>
#include <memory>
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
// Fixture: local HTTP server
// ===========================================================================

class HttpServerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<httplib::Server>();

        // Register a handler that returns a fixed IP address.
        server_->Get("/ip", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.set_content("198.51.100.42", "text/plain");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // Register a JSON endpoint.
        server_->Post("/update", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.set_content(R"({"status":"ok"})", "application/json");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // Register a PUT endpoint.
        server_->Put("/update", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.set_content(R"({"status":"updated"})", "application/json");
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // Register a DELETE endpoint.
        server_->Delete("/resource", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.status = 204;
            server_hit_count_.fetch_add(1, std::memory_order_relaxed);
        });

        // Register a plain-text endpoint for HttpIpSource error path (non-IP body).
        server_->Get("/not-an-ip", [&](const httplib::Request & /*req*/, httplib::Response &resp) {
            resp.set_content("this is not an ip address", "text/plain");
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
        server_->stop();
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
