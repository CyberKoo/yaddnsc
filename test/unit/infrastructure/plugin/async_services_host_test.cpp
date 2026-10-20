//
// Host-side async service boundary tests.
//
// The async services table is the only channel a plugin has into the host, so
// every entry has to reject what it cannot serve: a request outside a step, a
// request whose declared prefix does not cover the fields the host reads, and
// an operation id the host never handed out. These cases drive the table
// through its ABI entry points, the way a plugin would.
//

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "application/ports/log.h"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/async/services.h"
#include "infrastructure/plugin/detail/cycle.h"

namespace {

constexpr std::string_view kUrl{"https://example.invalid/update"};
constexpr std::string_view kContentType{"application/json"};

/// A logger that records what the host forwarded, so the level mapping and
/// the source location the plugin sent are observable.
class RecordingLogger final : public app::LoggerPort {
public:
    struct Record {
        app::LogLevel level;
        std::string message;
        std::string file;
        int line;
        std::string function;
    };

    mutable std::vector<Record> records;
    bool enabled = true;

    [[nodiscard]] bool is_enabled(app::LogLevel) const override { return enabled; }

    void log(app::LogLevel level, std::string_view message, const std::source_location&) const override {
        records.push_back({level, std::string(message), {}, 0, {}});
    }

    void log_explicit(app::LogLevel level, std::string_view message, std::string_view file, int line,
                      std::string_view function) const override {
        records.push_back({level, std::string(message), std::string(file), line, std::string(function)});
    }
};

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

std::string message_of(const yaddnsc_error& error) {
    return error.message.data == nullptr ? std::string{} : std::string(error.message.data, error.message.size);
}

struct AsyncServicesFixture {
    RecordingLogger logger;
    std::shared_ptr<plugin::CallState> state = std::make_shared<plugin::CallState>();
    plugin::HostAsyncServicesContext context{logger, state};
    plugin::AsyncStepCommands commands;

    AsyncServicesFixture() {
        context.set_phase(plugin::HostAsyncServicesContext::Phase::BEGIN);
        context.set_commands(&commands);
    }

    [[nodiscard]] yaddnsc_status start(const yaddnsc_http_request& request, uint64_t* op_id,
                                       yaddnsc_error* error) const {
        return context.services().http_start(context.services().context, &request, op_id, error);
    }

    [[nodiscard]] yaddnsc_status cancel(uint64_t op_id, yaddnsc_error* error) const {
        return context.services().http_cancel(context.services().context, op_id, error);
    }
};

}  // namespace

TEST(AsyncServicesHost, LogMapsEveryLevel) {
    AsyncServicesFixture fixture;
    static constexpr std::string_view message{"a plugin log record"};
    yaddnsc_source_location location{};
    location.file = {"plugin.cpp", 10};
    location.line = 42;
    location.function = {"update", 6};

    const std::pair<yaddnsc_log_level, app::LogLevel> expected[] = {
        {YADDNSC_LOG_TRACE, app::LogLevel::TRACE}, {YADDNSC_LOG_DEBUG, app::LogLevel::DEBUG},
        {YADDNSC_LOG_INFO, app::LogLevel::INFO},   {YADDNSC_LOG_WARN, app::LogLevel::WARN},
        {YADDNSC_LOG_ERROR, app::LogLevel::ERROR},
    };
    for (const auto& [level, mapped] : expected) {
        fixture.logger.records.clear();
        fixture.context.services().log(fixture.context.services().context, level, {message.data(), message.size()},
                                       &location);
        ASSERT_EQ(fixture.logger.records.size(), 1u);
        EXPECT_EQ(fixture.logger.records[0].level, mapped);
        EXPECT_EQ(fixture.logger.records[0].message, message);
        EXPECT_EQ(fixture.logger.records[0].file, "plugin.cpp");
        EXPECT_EQ(fixture.logger.records[0].line, 42);
        EXPECT_EQ(fixture.logger.records[0].function, "update");
    }
}

TEST(AsyncServicesHost, LogRecordsAnUnknownLevelAsInfo) {
    AsyncServicesFixture fixture;
    static constexpr std::string_view message{"a plugin log record"};

    fixture.context.services().log(fixture.context.services().context, 99, {message.data(), message.size()}, nullptr);

    ASSERT_EQ(fixture.logger.records.size(), 1u);
    EXPECT_EQ(fixture.logger.records[0].level, app::LogLevel::INFO);
    EXPECT_TRUE(fixture.logger.records[0].file.empty());
    EXPECT_EQ(fixture.logger.records[0].line, 0);
    EXPECT_TRUE(fixture.logger.records[0].function.empty());
}

TEST(AsyncServicesHost, LogSkipsADisabledLevel) {
    AsyncServicesFixture fixture;
    fixture.logger.enabled = false;
    static constexpr std::string_view message{"a plugin log record"};

    fixture.context.services().log(fixture.context.services().context, YADDNSC_LOG_ERROR,
                                   {message.data(), message.size()}, nullptr);

    EXPECT_TRUE(fixture.logger.records.empty());
}

TEST(AsyncServicesHost, LogEnabledAnswersForEveryLevel) {
    AsyncServicesFixture fixture;

    EXPECT_EQ(fixture.context.services().log_enabled(fixture.context.services().context, YADDNSC_LOG_TRACE), 1);
    EXPECT_EQ(fixture.context.services().log_enabled(fixture.context.services().context, YADDNSC_LOG_ERROR), 1);
    EXPECT_EQ(fixture.context.services().log_enabled(fixture.context.services().context, 99), 1);

    fixture.logger.enabled = false;
    EXPECT_EQ(fixture.context.services().log_enabled(fixture.context.services().context, YADDNSC_LOG_INFO), 0);
}

TEST(AsyncServicesHost, LogEnabledRejectsANullContext) {
    AsyncServicesFixture fixture;

    EXPECT_EQ(fixture.context.services().log_enabled(nullptr, YADDNSC_LOG_INFO), 0);
}

TEST(AsyncServicesHost, IsCancelledRejectsANullContext) {
    AsyncServicesFixture fixture;

    EXPECT_EQ(fixture.context.services().is_cancelled(nullptr), 0);
}

TEST(AsyncServicesHost, IsCancelledFollowsTheCycleToken) {
    AsyncServicesFixture fixture;

    EXPECT_EQ(fixture.context.services().is_cancelled(fixture.context.services().context), 0);
    fixture.state->cancelled.store(true);
    EXPECT_EQ(fixture.context.services().is_cancelled(fixture.context.services().context), 1);
}

TEST(AsyncServicesHost, HttpStartRejectsANullOpId) {
    AsyncServicesFixture fixture;
    auto request = make_request();
    yaddnsc_error error = make_error();

    const yaddnsc_status status =
        fixture.context.services().http_start(fixture.context.services().context, &request, nullptr, &error);

    EXPECT_EQ(status, YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "out_op_id must not be null");
}

TEST(AsyncServicesHost, HttpStartRejectsAMissingCommandSink) {
    AsyncServicesFixture fixture;
    fixture.context.clear_commands();
    auto request = make_request();
    yaddnsc_error error = make_error();
    uint64_t op_id = 0;

    EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "async HTTP command sink is not available");
    EXPECT_EQ(op_id, 0u);
}

TEST(AsyncServicesHost, HttpStartRejectsACallOutsideAStep) {
    AsyncServicesFixture fixture;
    fixture.context.set_phase(plugin::HostAsyncServicesContext::Phase::VALIDATE);
    auto request = make_request();
    yaddnsc_error error = make_error();
    uint64_t op_id = 0;

    EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "http_start is only available during async begin/resume");
}

TEST(AsyncServicesHost, HttpStartRejectsAMalformedRequest) {
    AsyncServicesFixture fixture;
    yaddnsc_error error = make_error();
    uint64_t op_id = 0;

    {
        auto request = make_request();
        request.struct_size = YADDNSC_HTTP_REQUEST_MIN_SIZE - 1;
        EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async HTTP request");
    }
    {
        auto request = make_request();
        request.url = {kUrl.data(), 0};
        EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async HTTP request");
    }
    {
        auto request = make_request();
        request.method = 99;
        EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async HTTP request");
    }
    {
        auto request = make_request();
        request.header_count = 1;
        request.headers = nullptr;
        EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async HTTP request");
    }
    {
        yaddnsc_http_header header{};
        header.name = {nullptr, 3};
        auto request = make_request();
        request.header_count = 1;
        request.headers = &header;
        EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async HTTP request");
    }
    {
        auto request = make_request();
        request.body = {nullptr, 4};
        EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "invalid async HTTP request");
    }
    EXPECT_TRUE(fixture.commands.starts.empty());
}

TEST(AsyncServicesHost, HttpStartRejectsACancelledCycle) {
    AsyncServicesFixture fixture;
    fixture.state->cancelled.store(true);
    auto request = make_request();
    yaddnsc_error error = make_error();
    uint64_t op_id = 0;

    EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(message_of(error), "the request was cancelled");
    EXPECT_EQ(op_id, 0u);
    EXPECT_TRUE(fixture.commands.starts.empty());
}

TEST(AsyncServicesHost, HttpStartAcceptsEveryMethod) {
    AsyncServicesFixture fixture;
    yaddnsc_error error = make_error();
    uint64_t op_id = 0;

    const std::pair<yaddnsc_http_method, http::Method> expected[] = {
        {YADDNSC_HTTP_GET, http::Method::GET},         {YADDNSC_HTTP_POST, http::Method::POST},
        {YADDNSC_HTTP_PUT, http::Method::PUT},         {YADDNSC_HTTP_DELETE, http::Method::DELETE},
        {YADDNSC_HTTP_PATCH, http::Method::PATCH},     {YADDNSC_HTTP_HEAD, http::Method::HEAD},
        {YADDNSC_HTTP_OPTIONS, http::Method::OPTIONS},
    };
    for (const auto& [method, mapped] : expected) {
        auto request = make_request();
        request.method = method;
        EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_OK);
        ASSERT_FALSE(fixture.commands.starts.empty());
        EXPECT_EQ(fixture.commands.starts.back().request.method, mapped);
        fixture.commands.clear();
    }
}

TEST(AsyncServicesHost, HttpStartCopiesTheRequest) {
    AsyncServicesFixture fixture;
    static constexpr std::string_view name{"X-Token"};
    static constexpr std::string_view value{"secret"};
    static constexpr std::string_view body{"{\"a\":1}"};
    yaddnsc_http_header header{};
    header.name = {name.data(), name.size()};
    header.value = {value.data(), value.size()};

    auto request = make_request();
    request.headers = &header;
    request.header_count = 1;
    request.body = {reinterpret_cast<const uint8_t*>(body.data()), body.size()};

    yaddnsc_error error = make_error();
    uint64_t op_id = 0;
    ASSERT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_OK);
    ASSERT_EQ(fixture.commands.starts.size(), 1u);

    const auto& command = fixture.commands.starts.front();
    EXPECT_EQ(command.op_id, 1u);
    EXPECT_EQ(op_id, 1u);
    EXPECT_EQ(command.url, kUrl);
    EXPECT_EQ(command.request.method, http::Method::POST);
    EXPECT_EQ(command.request.content_type, kContentType);
    ASSERT_TRUE(command.request.body.has_value());
    EXPECT_EQ(*command.request.body, body);
    ASSERT_EQ(command.request.headers.size(), 1u);
    EXPECT_EQ(command.request.headers.begin()->first, name);
    EXPECT_EQ(command.request.headers.begin()->second, value);
}

TEST(AsyncServicesHost, HttpCancelRejectsAMissingCommandSink) {
    AsyncServicesFixture fixture;
    fixture.context.clear_commands();
    yaddnsc_error error = make_error();

    EXPECT_EQ(fixture.cancel(1, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "async HTTP command sink is not available");
}

TEST(AsyncServicesHost, HttpCancelRejectsACallOutsideAStep) {
    AsyncServicesFixture fixture;
    fixture.context.set_phase(plugin::HostAsyncServicesContext::Phase::VALIDATE);
    yaddnsc_error error = make_error();

    EXPECT_EQ(fixture.cancel(1, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "http_cancel is only available during async begin/resume");
}

TEST(AsyncServicesHost, HttpCancelRejectsAnOperationTheHostNeverHandedOut) {
    AsyncServicesFixture fixture;
    yaddnsc_error error = make_error();

    EXPECT_EQ(fixture.cancel(0, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "unknown or already-delivered async op");

    EXPECT_EQ(fixture.cancel(77, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "unknown or already-delivered async op");
    EXPECT_TRUE(fixture.commands.cancels.empty());
}

TEST(AsyncServicesHost, HttpCancelRecordsTheOperationOnce) {
    AsyncServicesFixture fixture;
    auto request = make_request();
    yaddnsc_error error = make_error();
    uint64_t op_id = 0;
    ASSERT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_OK);

    EXPECT_EQ(fixture.cancel(op_id, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(fixture.cancel(op_id, &error), YADDNSC_STATUS_OK);
    ASSERT_EQ(fixture.commands.cancels.size(), 1u);
    EXPECT_EQ(fixture.commands.cancels.front(), op_id);
}

TEST(AsyncServicesHost, HttpCancelRejectsAnAlreadyDeliveredOperation) {
    AsyncServicesFixture fixture;
    auto request = make_request();
    yaddnsc_error error = make_error();
    uint64_t op_id = 0;
    ASSERT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_OK);

    fixture.context.mark_delivered(op_id);

    EXPECT_EQ(fixture.cancel(op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "unknown or already-delivered async op");
    EXPECT_TRUE(fixture.commands.cancels.empty());
}

TEST(AsyncServicesHost, HttpStartEntryRejectsANullContextOrRequest) {
    AsyncServicesFixture fixture;
    yaddnsc_error error = make_error();
    uint64_t op_id = 0xdeadbeef;  // NOLINT: sentinel; must be overwritten
    auto request = make_request();

    EXPECT_EQ(fixture.context.services().http_start(nullptr, &request, &op_id, &error),
              YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "context and request must not be null");
    EXPECT_EQ(op_id, 0u);

    EXPECT_EQ(fixture.context.services().http_start(fixture.context.services().context, nullptr, &op_id, &error),
              YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "context and request must not be null");
}

TEST(AsyncServicesHost, HttpCancelEntryRejectsANullContext) {
    AsyncServicesFixture fixture;
    yaddnsc_error error = make_error();

    EXPECT_EQ(fixture.context.services().http_cancel(nullptr, 1, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "context must not be null");
}

TEST(AsyncServicesHost, TheErrorIsLeftAloneWhenTheCallerHasNoRoom) {
    AsyncServicesFixture fixture;
    auto request = make_request();
    request.struct_size = YADDNSC_HTTP_REQUEST_MIN_SIZE - 1;
    yaddnsc_error error = make_error();
    error.struct_size = YADDNSC_ERROR_MIN_SIZE - 1;
    uint64_t op_id = 0;

    EXPECT_EQ(fixture.start(request, &op_id, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(error.struct_size, YADDNSC_ERROR_MIN_SIZE - 1);
    EXPECT_TRUE(error.message.data == nullptr);
}
