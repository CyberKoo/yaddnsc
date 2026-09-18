// Compile-time self-containment check for host-internal HTTP/transport headers.

#include <chrono>
#include <map>
#include <string>
#include <string_view>

#include <expected>
#include <gtest/gtest.h>

#include "infrastructure/network/http/client_port.h"
#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/transport/options.h"

namespace {

// Instantiate the internal types once so a missing include surfaces here.
class NoopHttpClient final : public HttpClient {
public:
    [[nodiscard]] std::expected<net::http::Response, net::http::Error> exchange(
        std::string_view,
        const net::http::Request&) const override {
        return net::http::Response{200, "ok", {}};
    }
};

TEST(HttpInternalHeaders, SelfContained) {
    const NoopHttpClient client;
    const net::http::Request request{.method = net::http::Method::GET};
    const auto result = client.exchange("https://example.com", request);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, 200);

    const net::http::Error error{net::http::ErrorCode::CANCELLED, "aborted"};
    EXPECT_EQ(error.code, net::http::ErrorCode::CANCELLED);

    const Transport::Options options{};
    EXPECT_EQ(options.connect_timeout, std::chrono::milliseconds{5000});

    const net::http::Options http_options{};
    EXPECT_TRUE(http_options.keep_alive);
}

}  // namespace
