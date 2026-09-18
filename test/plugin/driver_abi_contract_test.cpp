//
// Created by Kotarou on 2026/9/17.
//

/// Host-side contract tests for the v1 alpha plugin ABI.
///
/// Two angles:
///   - direct calls against the host services table (HostServicesContext)
///     for the fine-grained struct_size / NULL / canary matrix;
///   - cycles through the dlopen'ed whiteboard test plugin
///     (TEST_PLUGIN_PATH) for everything that must cross the .so boundary.
///
/// The contract test binary is a plain executable linked without -rdynamic:
/// every successful dlopen of the test plugin also proves the plugin needs
/// no host-exported symbols (no CORE_LOG backfill).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>
#include <stdint.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "application/ports/log.h"
#include "domain/error/error.h"
#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/plugin/host_services.h"
#include "infrastructure/plugin/plugin_loader.h"
#include "infrastructure/plugin/shared_library.h"
#include "plugin/plugin_test_doubles.h"
#include "support/util/cancellation_token.hpp"

namespace {

constexpr std::string_view kPluginPath = TEST_PLUGIN_PATH;

/// Output buffer with a canary tail: the host may write at most the declared
/// struct_size bytes and must never touch the unknown tail.
template<typename T, size_t Tail = 64>
struct Canary {
    T value{};
    std::array<std::byte, Tail> tail{};

    Canary() { tail.fill(std::byte{0xAA}); }

    void oversize() { value.struct_size = static_cast<uint32_t>(sizeof(T) + Tail); }

    [[nodiscard]] bool tail_intact() const {
        return std::ranges::all_of(tail, [](std::byte b) { return b == std::byte{0xAA}; });
    }
};

[[nodiscard]] yaddnsc_http_request make_http_request(std::string_view url,
                                                     yaddnsc_http_method method = YADDNSC_HTTP_GET) {
    yaddnsc_http_request request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.url = {url.data(), url.size()};
    request.method = method;
    return request;
}

[[nodiscard]] yaddnsc_error make_error_buffer() {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    return error;
}

/// The four raw entry points, resolved by hand so NULL arguments can be
/// passed (the PluginModule trampolines take references).
struct RawPlugin {
    SharedLibrary library;
    decltype(&yaddnsc_driver_get_descriptor) get_descriptor = nullptr;
    decltype(&yaddnsc_driver_create) create = nullptr;
    decltype(&yaddnsc_driver_destroy) destroy = nullptr;
    decltype(&yaddnsc_driver_update) update = nullptr;
};

[[nodiscard]] RawPlugin resolve_raw() {
    RawPlugin raw;
    auto library = SharedLibrary::open(std::string(kPluginPath));
    EXPECT_TRUE(library.has_value()) << library.error();
    if (!library) {
        return raw;
    }
    raw.library = std::move(*library);
    raw.get_descriptor =
        reinterpret_cast<decltype(raw.get_descriptor)>(raw.library.resolve("yaddnsc_driver_get_descriptor"));  // NOLINT
    raw.create = reinterpret_cast<decltype(raw.create)>(raw.library.resolve("yaddnsc_driver_create"));         // NOLINT
    raw.destroy = reinterpret_cast<decltype(raw.destroy)>(raw.library.resolve("yaddnsc_driver_destroy"));      // NOLINT
    raw.update = reinterpret_cast<decltype(raw.update)>(raw.library.resolve("yaddnsc_driver_update"));         // NOLINT
    return raw;
}

[[nodiscard]] std::shared_ptr<const PluginModule> load_module() {
    auto module = PluginModule::load(std::string(kPluginPath));
    EXPECT_TRUE(module.has_value()) << module.error().message;
    if (!module) {
        return nullptr;
    }
    return std::make_shared<const PluginModule>(std::move(*module));
}

}  // namespace

// ===========================================================================
//  get_descriptor / create / destroy / update — entry-point validation
// ===========================================================================

TEST(DriverAbiContract, GetDescriptorRejectsNullOut) {
    auto raw = resolve_raw();
    ASSERT_NE(raw.get_descriptor, nullptr);

    EXPECT_EQ(raw.get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);

    const yaddnsc_driver_descriptor* descriptor = nullptr;
    ASSERT_EQ(raw.get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->struct_size, sizeof(yaddnsc_driver_descriptor));
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "test_driver_plugin");
}

TEST(DriverAbiContract, CreateValidatesItsArguments) {
    auto raw = resolve_raw();
    ASSERT_NE(raw.create, nullptr);

    HostUpdateContext host;
    const auto services = host.context.make_services();

    yaddnsc_driver* handle = nullptr;
    yaddnsc_error error = make_error_buffer();

    // services == NULL
    EXPECT_EQ(raw.create(nullptr, &handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(handle, nullptr);
    // out_driver == NULL
    EXPECT_EQ(raw.create(&services, nullptr, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    // services struct_size below the required prefix
    auto small_services = services;
    small_services.struct_size = YADDNSC_HOST_SERVICES_MIN_SIZE - 1;
    EXPECT_EQ(raw.create(&small_services, &handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    // services api_revision mismatch
    auto wrong_revision = services;
    wrong_revision.api_revision = YADDNSC_DRIVER_API_REVISION + 1;
    EXPECT_EQ(raw.create(&wrong_revision, &handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    // null function pointers
    for (auto broken = services;;) {
        broken.log = nullptr;
        EXPECT_EQ(raw.create(&broken, &handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        broken = services;
        broken.http_exchange = nullptr;
        EXPECT_EQ(raw.create(&broken, &handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        broken = services;
        broken.is_cancelled = nullptr;
        EXPECT_EQ(raw.create(&broken, &handle, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        break;
    }

    // The happy path still works afterwards; destroy(NULL) is a no-op.
    ASSERT_EQ(raw.create(&services, &handle, &error), YADDNSC_STATUS_OK);
    ASSERT_NE(handle, nullptr);
    raw.destroy(handle);
    raw.destroy(nullptr);
}

TEST(DriverAbiContract, UpdateValidatesItsArguments) {
    auto raw = resolve_raw();
    ASSERT_NE(raw.create, nullptr);

    HostUpdateContext host;
    const auto services = host.context.make_services();

    yaddnsc_error error = make_error_buffer();
    yaddnsc_driver* handle = nullptr;
    ASSERT_EQ(raw.create(&services, &handle, &error), YADDNSC_STATUS_OK);
    ASSERT_NE(handle, nullptr);

    const auto request =
        make_update_request("192.0.2.1", "A", "example.com", "www", "www.example.com", R"({"op":"success"})");

    // driver == NULL / request == NULL
    EXPECT_EQ(raw.update(nullptr, &request, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(raw.update(handle, nullptr, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    // struct_size == 0 and one below the minimum
    auto zero_size = request;
    zero_size.struct_size = 0;
    EXPECT_EQ(raw.update(handle, &zero_size, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    auto below_min = request;
    below_min.struct_size = YADDNSC_UPDATE_REQUEST_MIN_SIZE - 1;
    EXPECT_EQ(raw.update(handle, &below_min, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    // exactly the minimum covers the whole struct → accepted
    auto at_min = request;
    at_min.struct_size = YADDNSC_UPDATE_REQUEST_MIN_SIZE;
    EXPECT_EQ(raw.update(handle, &at_min, &error), YADDNSC_STATUS_OK);

    raw.destroy(handle);
}

// ===========================================================================
//  http_exchange — NULL / struct_size matrix (direct table calls)
// ===========================================================================

TEST(DriverAbiContract, HttpExchangeRejectsNullPointers) {
    HostUpdateContext host;
    const auto services = host.context.make_services();

    const auto request = make_http_request("http://localhost/x");
    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    yaddnsc_error error = make_error_buffer();

    EXPECT_EQ(services.http_exchange(nullptr, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(services.http_exchange(services.context, nullptr, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(services.http_exchange(services.context, &request, nullptr, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    // out_error is optional: a valid call with NULL out_error succeeds.
    host.client.queue_response(200, "ok");
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, nullptr), YADDNSC_STATUS_OK);
    EXPECT_EQ(response.status_code, 200u);
}

TEST(DriverAbiContract, HttpExchangeRequestStructSizeMatrix) {
    HostUpdateContext host;
    const auto services = host.context.make_services();

    yaddnsc_http_response response{};
    yaddnsc_error error = make_error_buffer();

    // struct_size == 0 → rejected
    auto request = make_http_request("http://localhost/x");
    request.struct_size = 0;
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    // one below the minimum → rejected
    request.struct_size = YADDNSC_HTTP_REQUEST_MIN_SIZE - 1;
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    // exactly the minimum covers every v1 field → accepted
    host.client.queue_response(200, "min");
    request.struct_size = YADDNSC_HTTP_REQUEST_MIN_SIZE;
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(response.status_code, 200u);

    // larger than sizeof with a canary tail → accepted; the host must not
    // write into the (input-only) request at all, and the tail stays intact.
    Canary<yaddnsc_http_request> big;
    big.value = make_http_request("http://localhost/x");
    big.oversize();
    host.client.queue_response(200, "big");
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    EXPECT_EQ(services.http_exchange(services.context, &big.value, &response, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(response.status_code, 200u);
    EXPECT_TRUE(big.tail_intact());
    EXPECT_EQ(big.value.struct_size, sizeof(yaddnsc_http_request) + 64u);
}

TEST(DriverAbiContract, HttpExchangeResponseStructSizeMatrix) {
    HostUpdateContext host;
    const auto services = host.context.make_services();
    const auto request = make_http_request("http://localhost/x");
    yaddnsc_error error = make_error_buffer();

    // struct_size == 0 → rejected (no exchange happens)
    yaddnsc_http_response response{};
    response.struct_size = 0;
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(host.client.request_count(), 0u);

    // one below the minimum → rejected
    response.struct_size = YADDNSC_HTTP_RESPONSE_MIN_SIZE - 1;
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(host.client.request_count(), 0u);

    // exactly the minimum covers every v1 field → accepted
    host.client.queue_response(200, "min");
    response.struct_size = YADDNSC_HTTP_RESPONSE_MIN_SIZE;
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(response.status_code, 200u);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(response.body.data), response.body.size), "min");

    // larger than sizeof with a canary tail → accepted; the writer clamps
    // struct_size to sizeof and never touches the unknown tail.
    Canary<yaddnsc_http_response> big;
    big.oversize();
    host.client.queue_response(200, "big");
    EXPECT_EQ(services.http_exchange(services.context, &request, &big.value, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(big.value.status_code, 200u);
    EXPECT_EQ(big.value.struct_size, sizeof(yaddnsc_http_response));
    EXPECT_TRUE(big.tail_intact());
}

TEST(DriverAbiContract, HttpExchangeErrorStructSizeMatrix) {
    HostUpdateContext host;
    const auto services = host.context.make_services();
    const auto request = make_http_request("http://localhost/x");
    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));

    // struct_size == 0 → the status is still returned, nothing is written.
    host.client.queue_error(net::http::ErrorCode::TIMEOUT, "timed out");
    yaddnsc_error zero_error{};
    std::memset(&zero_error, 0x55, sizeof(zero_error));
    zero_error.struct_size = 0;
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &zero_error), YADDNSC_STATUS_NETWORK_ERROR);
    yaddnsc_error untouched{};
    std::memset(&untouched, 0x55, sizeof(untouched));
    untouched.struct_size = 0;
    EXPECT_EQ(std::memcmp(&zero_error, &untouched, sizeof(zero_error)), 0);

    // one below the minimum → same: returned, not written.
    host.client.queue_error(net::http::ErrorCode::TIMEOUT, "timed out");
    yaddnsc_error below_min{};
    std::memset(&below_min, 0x55, sizeof(below_min));
    below_min.struct_size = YADDNSC_ERROR_MIN_SIZE - 1;
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &below_min), YADDNSC_STATUS_NETWORK_ERROR);
    yaddnsc_error untouched2{};
    std::memset(&untouched2, 0x55, sizeof(untouched2));
    untouched2.struct_size = YADDNSC_ERROR_MIN_SIZE - 1;
    EXPECT_EQ(std::memcmp(&below_min, &untouched2, sizeof(below_min)), 0);

    // oversized with a canary tail → status/message written, struct_size
    // clamped to sizeof, the unknown tail untouched.
    Canary<yaddnsc_error> big;
    big.oversize();
    host.client.queue_error(net::http::ErrorCode::CONNECT_FAILED, "refused");
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &big.value), YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(big.value.status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(std::string_view(big.value.message.data, big.value.message.size), "refused");
    EXPECT_EQ(big.value.retry_after_seconds, 0u);
    EXPECT_EQ(big.value.struct_size, sizeof(yaddnsc_error));
    EXPECT_TRUE(big.tail_intact());
}

TEST(DriverAbiContract, HttpExchangeRejectsMalformedLeafViews) {
    HostUpdateContext host;
    const auto services = host.context.make_services();
    yaddnsc_http_response response{};
    yaddnsc_error error = make_error_buffer();

    // empty / null url
    auto request = make_http_request("");
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INVALID_ARGUMENT);

    request = make_http_request("http://localhost/x");
    request.url = {nullptr, 5};  // size > 0 with NULL data is invalid
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    // unknown method constants
    for (const yaddnsc_http_method method : {UINT32_C(0), UINT32_C(99)}) {
        request = make_http_request("http://localhost/x", method);
        EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error),
                  YADDNSC_STATUS_INVALID_ARGUMENT);
    }

    // header_count > 0 with NULL headers
    request = make_http_request("http://localhost/x");
    request.headers = nullptr;
    request.header_count = 1;
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    // body size > 0 with NULL body data
    request = make_http_request("http://localhost/x");
    request.body = {nullptr, 3};
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    // Nothing malformed may reach the transport.
    EXPECT_EQ(host.client.request_count(), 0u);

    // The legal NULL combinations work: header_count == 0 with NULL headers,
    // no body at all.
    request = make_http_request("http://localhost/x");
    host.client.queue_response(204, "");
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(response.status_code, 204u);
}

TEST(DriverAbiContract, HttpMethodMapping) {
    HostUpdateContext host;
    const auto services = host.context.make_services();

    const std::array mapping{
        std::pair{YADDNSC_HTTP_GET, net::http::Method::GET},
        std::pair{YADDNSC_HTTP_POST, net::http::Method::POST},
        std::pair{YADDNSC_HTTP_PUT, net::http::Method::PUT},
        std::pair{YADDNSC_HTTP_DELETE, net::http::Method::DEL},
        std::pair{YADDNSC_HTTP_PATCH, net::http::Method::PATCH},
        std::pair{YADDNSC_HTTP_HEAD, net::http::Method::HEAD},
        std::pair{YADDNSC_HTTP_OPTIONS, net::http::Method::OPTIONS},
    };

    for (const auto& [abi_method, expected] : mapping) {
        const auto request = make_http_request("http://localhost/x", abi_method);
        yaddnsc_http_response response{};
        response.struct_size = static_cast<uint32_t>(sizeof(response));
        yaddnsc_error error = make_error_buffer();

        host.client.queue_response(200, "ok");
        ASSERT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_OK)
            << "abi method " << abi_method;
    }

    const auto captured = host.client.requests();
    ASSERT_EQ(captured.size(), mapping.size());
    for (size_t i = 0; i < mapping.size(); ++i) {
        EXPECT_EQ(captured[i].method, mapping[i].second);
    }
}

// ===========================================================================
//  http_exchange — semantics (errors, 4xx, view lifetime, field pass-through)
// ===========================================================================

TEST(DriverAbiContract, TransportErrorMapping) {
    HostUpdateContext host;
    const auto services = host.context.make_services();
    const auto request = make_http_request("http://localhost/x");

    // CANCELLED maps to CANCELLED…
    host.client.queue_error(net::http::ErrorCode::CANCELLED, "aborted");
    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    yaddnsc_error error = make_error_buffer();
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(std::string_view(error.message.data, error.message.size), "aborted");

    // …every other net::http error maps to NETWORK_ERROR.
    for (const auto code : {net::http::ErrorCode::TIMEOUT, net::http::ErrorCode::CONNECT_FAILED,
                            net::http::ErrorCode::TLS_HANDSHAKE_FAILED, net::http::ErrorCode::RESPONSE_PARSE_FAILED}) {
        host.client.queue_error(code, "boom");
        response.struct_size = static_cast<uint32_t>(sizeof(response));
        error = make_error_buffer();
        EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_NETWORK_ERROR)
            << "code " << static_cast<int>(code);
        EXPECT_EQ(std::string_view(error.message.data, error.message.size), "boom");
    }
}

TEST(DriverAbiContract, Http4xxIsDeliveredAsResponse) {
    HostUpdateContext host;
    const auto services = host.context.make_services();
    const auto request = make_http_request("http://localhost/x");

    host.client.queue_response(404, "not found", {{"Server", "unit-test"}});
    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    yaddnsc_error error = make_error_buffer();

    ASSERT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(response.status_code, 404u);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(response.body.data), response.body.size), "not found");
    ASSERT_EQ(response.header_count, 1u);
    EXPECT_EQ(std::string_view(response.headers[0].name.data, response.headers[0].name.size), "Server");
    EXPECT_EQ(std::string_view(response.headers[0].value.data, response.headers[0].value.size), "unit-test");
}

TEST(DriverAbiContract, EarlierResponseViewsSurviveLaterExchanges) {
    HostUpdateContext host;
    const auto services = host.context.make_services();
    const auto request = make_http_request("http://localhost/x");

    host.client.queue_response(200, "first-body", {{"X-One", "one"}});
    yaddnsc_http_response first{};
    first.struct_size = static_cast<uint32_t>(sizeof(first));
    yaddnsc_error error = make_error_buffer();
    ASSERT_EQ(services.http_exchange(services.context, &request, &first, &error), YADDNSC_STATUS_OK);

    host.client.queue_response(200, "second-body-with-different-length", {{"X-Two", "two"}});
    yaddnsc_http_response second{};
    second.struct_size = static_cast<uint32_t>(sizeof(second));
    ASSERT_EQ(services.http_exchange(services.context, &request, &second, &error), YADDNSC_STATUS_OK);

    // The first response's views must still byte-compare equal.
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(first.body.data), first.body.size), "first-body");
    ASSERT_EQ(first.header_count, 1u);
    EXPECT_EQ(std::string_view(first.headers[0].name.data, first.headers[0].name.size), "X-One");
    EXPECT_EQ(std::string_view(first.headers[0].value.data, first.headers[0].value.size), "one");
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(second.body.data), second.body.size),
              "second-body-with-different-length");
}

TEST(DriverAbiContract, RequestFieldsReachTheTransport) {
    HostUpdateContext host;
    const auto services = host.context.make_services();

    const std::string url = "http://localhost/submit";
    const std::string body = "payload-bytes";
    const yaddnsc_http_header headers[] = {
        {{"Authorization", 13}, {"Bearer tok", 10}},
        {{"X-Custom", 8}, {"yes", 3}},
    };

    yaddnsc_http_request request = make_http_request(url, YADDNSC_HTTP_POST);
    request.headers = headers;
    request.header_count = 2;
    request.body = {reinterpret_cast<const uint8_t*>(body.data()), body.size()};
    const std::string_view content_type = "application/x-test";
    request.content_type = {content_type.data(), content_type.size()};

    host.client.queue_response(200, "ok");
    yaddnsc_http_response response{};
    response.struct_size = static_cast<uint32_t>(sizeof(response));
    yaddnsc_error error = make_error_buffer();
    ASSERT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_OK);

    const auto captured = host.client.requests();
    ASSERT_EQ(captured.size(), 1u);
    EXPECT_EQ(captured[0].url, url);
    EXPECT_EQ(captured[0].method, net::http::Method::POST);
    EXPECT_EQ(captured[0].headers.count("Authorization"), 1u);
    EXPECT_EQ(captured[0].headers.find("Authorization")->second, "Bearer tok");
    EXPECT_EQ(captured[0].headers.find("X-Custom")->second, "yes");
    ASSERT_TRUE(captured[0].body.has_value());
    EXPECT_EQ(*captured[0].body, body);
    EXPECT_EQ(captured[0].content_type, content_type);
}

// ===========================================================================
//  log — level mapping, source location, tolerance of broken locations
// ===========================================================================

TEST(DriverAbiContract, LogLevelMapping) {
    HostUpdateContext host;
    const auto services = host.context.make_services();
    const yaddnsc_source_location location{{"p.cpp", 5}, 1, {"f", 1}};
    const yaddnsc_string message{"m", 1};

    for (const yaddnsc_log_level level :
         {YADDNSC_LOG_TRACE, YADDNSC_LOG_DEBUG, YADDNSC_LOG_INFO, YADDNSC_LOG_WARN, YADDNSC_LOG_ERROR}) {
        services.log(services.context, level, message, &location);
    }
    // An unknown level must not crash; it degrades to info.
    services.log(services.context, UINT32_C(99), message, &location);

    const auto records = host.logger.records();
    ASSERT_EQ(records.size(), 6u);
    EXPECT_EQ(records[0].level, LogLevel::trace);
    EXPECT_EQ(records[1].level, LogLevel::debug);
    EXPECT_EQ(records[2].level, LogLevel::info);
    EXPECT_EQ(records[3].level, LogLevel::warn);
    EXPECT_EQ(records[4].level, LogLevel::error);
    EXPECT_EQ(records[5].level, LogLevel::info);
}

TEST(DriverAbiContract, LogCarriesSourceLocation) {
    HostUpdateContext host;
    const auto services = host.context.make_services();
    const yaddnsc_source_location location{{"plugin.cpp", 10}, 42, {"do_update", 9}};
    const yaddnsc_string message{"hello", 5};

    services.log(services.context, YADDNSC_LOG_WARN, message, &location);

    const auto records = host.logger.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].level, LogLevel::warn);
    EXPECT_EQ(records[0].message, "hello");
    EXPECT_EQ(records[0].file, "plugin.cpp");
    EXPECT_EQ(records[0].line, 42);
    EXPECT_EQ(records[0].function, "do_update");
}

TEST(DriverAbiContract, LogToleratesBrokenLocations) {
    HostUpdateContext host;
    const auto services = host.context.make_services();
    const yaddnsc_string message{"m", 1};

    // NULL location, empty file, line <= 0, empty function: none of these may
    // crash or fail anything — logging is best-effort by contract.
    services.log(services.context, YADDNSC_LOG_INFO, message, nullptr);
    const yaddnsc_source_location no_file{{nullptr, 0}, 3, {"f", 1}};
    services.log(services.context, YADDNSC_LOG_INFO, message, &no_file);
    const yaddnsc_source_location zero_line{{"f.cpp", 5}, 0, {"f", 1}};
    services.log(services.context, YADDNSC_LOG_INFO, message, &zero_line);

    const auto records = host.logger.records();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_TRUE(records[0].file.empty());
    EXPECT_EQ(records[0].line, 0);
    EXPECT_TRUE(records[0].function.empty());
}

// ===========================================================================
//  Plugin-mediated contracts (across the .so boundary)
// ===========================================================================

TEST(DriverAbiContract, PluginReceivesUpdateParameters) {
    auto module = load_module();
    ASSERT_NE(module, nullptr);

    HostUpdateContext host;
    const auto services = host.context.make_services();
    const auto result = run_module_cycle(*module, services, R"({"op":"echo_params"})");

    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.update_status, YADDNSC_STATUS_OK) << result.error_message;

    const auto records = host.logger.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].message,
              R"(params ip=192.0.2.1 rd=A domain=example.com sub=www fqdn=www.example.com param={"op":"echo_params"})");
}

TEST(DriverAbiContract, SdkLogMacroDeliversCallSiteLocation) {
    auto module = load_module();
    ASSERT_NE(module, nullptr);

    HostUpdateContext host;
    const auto services = host.context.make_services();
    const auto result = run_module_cycle(*module, services, R"({"op":"log_macro","message":"via sdk"})");
    ASSERT_EQ(result.update_status, YADDNSC_STATUS_OK) << result.error_message;

    const auto records = host.logger.records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].level, LogLevel::info);
    EXPECT_EQ(records[0].message, "via sdk");
    // The SDK macro path must carry all three of file / line / function.
    EXPECT_TRUE(records[0].file.ends_with("test_driver_plugin.cpp")) << records[0].file;
    EXPECT_GT(records[0].line, 0);
    EXPECT_EQ(records[0].function, "update");
}

TEST(DriverAbiContract, PluginRawLogViolationsDoNotFailUpdate) {
    auto module = load_module();
    ASSERT_NE(module, nullptr);

    HostUpdateContext host;
    const auto services = host.context.make_services();
    const auto result = run_module_cycle(*module, services, R"({"op":"log_raw"})");

    // Broken hand-written locations never turn the update into a failure.
    EXPECT_EQ(result.update_status, YADDNSC_STATUS_OK) << result.error_message;

    const auto records = host.logger.records();
    ASSERT_EQ(records.size(), 6u);
    const std::array expected_levels{LogLevel::info,  LogLevel::warn,  LogLevel::error,
                                     LogLevel::debug, LogLevel::trace, LogLevel::info};
    for (size_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(records[i].level, expected_levels[i]) << i;
        EXPECT_EQ(records[i].message, "raw log record") << i;
    }
    EXPECT_TRUE(records[0].file.empty());  // location == NULL
    EXPECT_TRUE(records[1].file.empty());  // empty file view
    EXPECT_EQ(records[2].line, 0);         // line == 0 passed through
    EXPECT_EQ(records[3].line, -3);        // negative line passed through
}

TEST(DriverAbiContract, PluginResponseViewsSurviveMultipleExchanges) {
    auto module = load_module();
    ASSERT_NE(module, nullptr);

    HostUpdateContext host;
    host.client.queue_response(200, "body-0", {{"X-Reply", "reply-0"}});
    host.client.queue_response(201, "body-1-longer", {{"X-Reply", "reply-1"}});
    host.client.queue_response(202, "body-2", {{"X-Reply", "reply-2"}});

    const auto services = host.context.make_services();
    const auto result = run_module_cycle(*module, services, R"({"op":"exchange","http_count":3})");

    // The plugin itself re-verified every earlier view after each exchange.
    EXPECT_EQ(result.update_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(host.client.request_count(), 3u);
    EXPECT_EQ(host.client.remaining(), 0u);
}

TEST(DriverAbiContract, CancellationReachesThePlugin) {
    auto module = load_module();
    ASSERT_NE(module, nullptr);

    Utils::CancellationSource source;

    // Not triggered, plugin expects "not cancelled".
    {
        HostUpdateContext host;
        host.token = source.token();
        HostServicesContext context(host.client, host.logger, host.token);
        const auto services = context.make_services();
        const auto result = run_module_cycle(*module, services, R"({"op":"check_cancel","expect_cancelled":false})");
        EXPECT_EQ(result.update_status, YADDNSC_STATUS_OK) << result.error_message;
    }

    source.trigger();

    // Triggered, plugin observes the cancellation.
    {
        HostUpdateContext host;
        HostServicesContext context(host.client, host.logger, source.token());
        const auto services = context.make_services();
        const auto result = run_module_cycle(*module, services, R"({"op":"check_cancel","expect_cancelled":true})");
        EXPECT_EQ(result.update_status, YADDNSC_STATUS_OK) << result.error_message;
    }
}

TEST(DriverAbiContract, ErrorReportIsCopiedSynchronously) {
    auto module = load_module();
    ASSERT_NE(module, nullptr);

    HostUpdateContext host;
    const auto services = host.context.make_services();
    const auto result = run_module_cycle(
        *module, services, R"({"op":"fail","status":"rate_limited","message":"slow down","retry_after":123})");

    EXPECT_EQ(result.update_status, YADDNSC_STATUS_RATE_LIMITED);
    EXPECT_EQ(result.error_message, "slow down");
    EXPECT_EQ(result.retry_after_seconds, 123u);
}

TEST(DriverAbiContract, ConcurrentUpdateErrorsAreIsolated) {
    auto module = load_module();
    ASSERT_NE(module, nullptr);

    // Two instances of the same module, updated on two threads with distinct
    // failure messages: neither message may leak into the other instance.
    std::array<ModuleCycleResult, 2> results{};
    std::array<std::string, 2> params = {R"({"op":"fail","status":"network_error","message":"thread-a-error"})",
                                         R"({"op":"fail","status":"network_error","message":"thread-b-error"})"};

    std::array<std::thread, 2> threads{};
    for (size_t i = 0; i < 2; ++i) {
        threads[i] = std::thread([&, i] {
            HostUpdateContext host;
            const auto services = host.context.make_services();
            results[i] = run_module_cycle(*module, services, params[i]);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(results[0].update_status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(results[1].update_status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(results[0].error_message, "thread-a-error");
    EXPECT_EQ(results[1].error_message, "thread-b-error");
}

TEST(DriverAbiContract, ConcurrentCreateFailuresAreIsolated) {
    auto module = load_module();
    ASSERT_NE(module, nullptr);

    // Arm two injected create() failures through the test control export.
    auto control = SharedLibrary::open(std::string(kPluginPath));
    ASSERT_TRUE(control.has_value()) << control.error();
    auto* set_failures =
        reinterpret_cast<void (*)(int)>(control->resolve("test_plugin_set_create_failures"));  // NOLINT
    ASSERT_NE(set_failures, nullptr);
    set_failures(2);

    std::array<yaddnsc_status, 2> statuses{};
    std::array<std::string, 2> messages{};
    std::array<std::thread, 2> threads{};
    for (size_t i = 0; i < 2; ++i) {
        threads[i] = std::thread([&, i] {
            HostUpdateContext host;
            const auto services = host.context.make_services();
            yaddnsc_error error = make_error_buffer();
            yaddnsc_driver* handle = nullptr;
            statuses[i] = module->create(services, &handle, error);
            if (error.message.data != nullptr) {
                messages[i] = std::string(error.message.data, error.message.size);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    set_failures(0);

    EXPECT_EQ(statuses[0], YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(statuses[1], YADDNSC_STATUS_INTERNAL_ERROR);
    // Each thread must receive its own ticket message — never the other's.
    const std::vector<std::string> sorted = {std::min(messages[0], messages[1]), std::max(messages[0], messages[1])};
    EXPECT_EQ(sorted[0], "injected create failure #1");
    EXPECT_EQ(sorted[1], "injected create failure #2");
}
