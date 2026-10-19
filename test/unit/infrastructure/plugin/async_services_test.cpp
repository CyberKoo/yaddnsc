//
// Host-side async service boundary tests: http_start must reject a request
// whose declared prefix does not cover the fields it would read, and must
// clear the writable op id on every failure.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "infrastructure/plugin/async/services.h"
#include "infrastructure/plugin/detail/cycle.h"
#include "mocks/null_logger.h"

namespace {

constexpr std::string_view kUrl{"https://example.invalid/update"};
constexpr std::string_view kContentType{"application/json"};

yaddnsc_http_request make_request() {
    yaddnsc_http_request request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    request.url = {kUrl.data(), kUrl.size()};
    request.method = YADDNSC_HTTP_POST;
    request.content_type = {kContentType.data(), kContentType.size()};
    return request;
}

yaddnsc_error make_error() {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    return error;
}

struct AsyncServicesFixture {
    NullLogger logger;
    std::shared_ptr<plugin::CallState> state = std::make_shared<plugin::CallState>();
    plugin::HostAsyncServicesContext context{logger, state};
    plugin::AsyncStepCommands commands;

    AsyncServicesFixture() {
        context.set_phase(plugin::HostAsyncServicesContext::Phase::BEGIN);
        context.set_commands(&commands);
    }
};

}  // namespace

TEST(AsyncServices, HttpStartRejectsAShortRequestAndClearsOpId) {
    AsyncServicesFixture fixture;
    auto request = make_request();
    request.struct_size = YADDNSC_HTTP_REQUEST_MIN_SIZE - 1;
    yaddnsc_error error = make_error();
    uint64_t op_id = 0xdeadbeef;  // NOLINT: sentinel; must be overwritten

    const yaddnsc_status status =
        fixture.context.services().http_start(fixture.context.services().context, &request, &op_id, &error);

    EXPECT_EQ(status, YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(op_id, 0u);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_TRUE(fixture.commands.starts.empty());
}

TEST(AsyncServices, HttpStartEntryClearsOpIdWhenTheRequestIsNull) {
    AsyncServicesFixture fixture;
    yaddnsc_error error = make_error();
    uint64_t op_id = 0xdeadbeef;  // NOLINT: sentinel; must be overwritten

    const yaddnsc_status status =
        fixture.context.services().http_start(fixture.context.services().context, nullptr, &op_id, &error);

    EXPECT_EQ(status, YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(op_id, 0u);
}

TEST(AsyncServices, HttpStartStillAcceptsACompleteRequest) {
    AsyncServicesFixture fixture;
    auto request = make_request();
    yaddnsc_error error = make_error();
    uint64_t op_id = 0;

    const yaddnsc_status status =
        fixture.context.services().http_start(fixture.context.services().context, &request, &op_id, &error);

    EXPECT_EQ(status, YADDNSC_STATUS_OK);
    EXPECT_NE(op_id, 0u);
    ASSERT_EQ(fixture.commands.starts.size(), 1u);
    EXPECT_EQ(fixture.commands.starts[0].op_id, op_id);
    EXPECT_EQ(fixture.commands.starts[0].url, kUrl);
}
