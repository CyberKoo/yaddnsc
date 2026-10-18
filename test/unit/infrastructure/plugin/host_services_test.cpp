//
// Unit tests for src/infrastructure/plugin/host_services.cpp
//
// Covers the host half of the plugin ABI firewall: the log-level and
// HTTP-method mappings, the C-frame trampolines the context installs in the
// services table, the argument and struct_size boundaries of http_exchange,
// and the exception-to-status translation.
//
// Everything runs in memory. No plugin is dlopen'd and no socket or file is
// touched: the Bridge a context borrows is bound to a root group that has
// already been joined, so a case that wrongly reached the bridge would be
// rejected instead of issuing a request. The few cases that need a host
// defect arm a test-local allocator fault for exactly one allocation.
// =============================================================================

#include "infrastructure/plugin/host_services.h"

#include <array>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "application/ports/log.h"
#include "coro/cancel_scope.h"
#include "coro/loop.h"
#include "coro/run.hpp"
#include "coro/task.hpp"
#include "coro/with_nursery.hpp"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/bridge.h"
#include "mocks/allocation_fault.h"
#include "yaddnsc/sdk/driver_abi.h"

namespace coro {
class Nursery;
}  // namespace coro

namespace {

/// Fault raised by the next allocation, while the injection window is open.
enum class Fault {
    none,
    std_exception,       ///< std::exception: reported through copy_callback_error
    long_std_exception,  ///< as above, but longer than the fixed callback buffer
    foreign_exception,   ///< not a std::exception: reaches the catch-all arm
    cancelled,           ///< coro::Cancelled, the runtime's own control flow
};

/// Deliberately unrelated to std::exception, so only `catch (...)` sees it.
struct HostDefect {};

/// The C firewall reports through a thread-local buffer; anything longer is
/// clamped. Only the test needs to know its size.
constexpr std::size_t kCallbackBufferCapacity = 512;

thread_local Fault armed_fault = Fault::none;
coro::CancelScope* cancellation_origin = nullptr;

void raise_fault(Fault fault) {
    switch (fault) {
        case Fault::std_exception:
            throw std::runtime_error("injected host defect");
        case Fault::long_std_exception:
            throw std::runtime_error(std::string(2 * kCallbackBufferCapacity, 'x'));
        case Fault::foreign_exception:
            throw HostDefect{};
        case Fault::cancelled:
            // The only public way to produce coro::Cancelled; it is sticky, so
            // the call always throws.
            if (cancellation_origin != nullptr) {
                cancellation_origin->throw_if_cancelled();
            }
            break;
        case Fault::none:
            break;
    }
    std::terminate();
}

void allocation_hook(std::size_t /*size*/) {
    if (armed_fault != Fault::none) {
        // Disarm first: raising the fault allocates too.
        raise_fault(std::exchange(armed_fault, Fault::none));
    }
}

/// Arms `fault` for the very next allocation on this thread. Keep the window
/// as narrow as the call under test.
class InjectFault {
public:
    explicit InjectFault(Fault fault) noexcept { armed_fault = fault; }

    InjectFault(const InjectFault&) = delete;
    InjectFault& operator=(const InjectFault&) = delete;

    ~InjectFault() noexcept { armed_fault = Fault::none; }
};

// Arm this binary's allocation-fault hook before any test runs.
[[maybe_unused]] const bool allocation_hook_armed = [] {
    test::set_allocation_fault_hook(&allocation_hook);
    return true;
}();

}  // namespace

namespace {

// ── Fixtures and request builders ──────────────────────────────────────────

constexpr std::string_view kUrl{"https://example.invalid/update"};
constexpr std::string_view kContentType{"application/json"};
constexpr std::string_view kRecordName{"X-Token"};
constexpr std::string_view kRecordValue{"secret"};
constexpr yaddnsc_http_header kValidHeaders[]{
    {{kRecordName.data(), kRecordName.size()}, {kRecordValue.data(), kRecordValue.size()}}};
/// A header whose value has a length without storage: the mandatory validation
/// boundary must reject it before any string_view is built from it.
constexpr yaddnsc_http_header kInvalidHeaders[]{
    {{"X-Token", 7}, {nullptr, 4}},
};

/// A request that passes every host boundary check.
[[nodiscard]] yaddnsc_http_request valid_request() {
    yaddnsc_http_request request{};
    request.struct_size = sizeof(request);
    request.url = {kUrl.data(), kUrl.size()};
    request.method = YADDNSC_HTTP_POST;
    request.content_type = {kContentType.data(), kContentType.size()};
    return request;
}

/// A valid request reshaped by `mutate` into one the host must reject.
template<typename Mutate>
[[nodiscard]] yaddnsc_http_request malformed(Mutate mutate) {
    auto request = valid_request();
    mutate(request);
    return request;
}

[[nodiscard]] yaddnsc_http_response sized_response() {
    yaddnsc_http_response response{};
    response.struct_size = sizeof(response);
    return response;
}

[[nodiscard]] yaddnsc_error sized_error() {
    yaddnsc_error error{};
    error.struct_size = sizeof(error);
    return error;
}

/// Read a written error message only when write_error actually stored one.
[[nodiscard]] std::string_view message_of(const yaddnsc_error& error) {
    if (error.message.data == nullptr) {
        return {};
    }
    return {error.message.data, error.message.size};
}

/// A Bridge whose root group is already joined. HostServicesContext only
/// borrows it, and every case here returns before an exchange could be
/// posted, so its closed group is never observed.
[[nodiscard]] std::unique_ptr<plugin::Bridge> make_closed_bridge(coro::Loop& loop) {
    std::unique_ptr<plugin::Bridge> bridge;
    auto root = [&]() -> coro::Task<void> {
        co_await coro::with_nursery(
            [&](coro::Nursery& group) -> coro::Task<void> {
                bridge = std::make_unique<plugin::Bridge>(loop, group, http::Options{}, std::chrono::seconds{1});
                co_return;
            },
            coro::NurseryPolicy::SUPERVISOR);
    };
    coro::run(loop, root());
    return bridge;
}

/// Recording logger that can also fail, so the C-frame firewall around
/// logging is observable. Mirrors the plugin contract tests, which need the
/// plain-data source location as well.
class ArmedRecordingLogger final : public app::LoggerPort {
public:
    struct Record {
        app::LogLevel level;
        std::string message;
        std::string file;
        int line;
        std::string function;
    };

    [[nodiscard]] bool is_enabled(app::LogLevel level) const override {
        return !(disable_debug && level == app::LogLevel::DEBUG);
    }

    void log(app::LogLevel level, std::string_view message, const std::source_location&) const override {
        log_explicit(level, message, {}, 0, {});
    }

    void log_explicit(app::LogLevel level, std::string_view message, std::string_view file, int line,
                      std::string_view function) const override {
        if (armed) {
            throw std::runtime_error("injected logger defect");
        }
        records_.push_back(Record{level, std::string(message), std::string(file), line, std::string(function)});
    }

    [[nodiscard]] const std::vector<Record>& records() const { return records_; }

    mutable bool armed = false;
    mutable bool disable_debug = false;

private:
    mutable std::vector<Record> records_;
};

class HostServicesTest : public ::testing::Test {
protected:
    void SetUp() override {
        bridge_ = make_closed_bridge(loop_);
        state_ = std::make_shared<plugin::CallState>();
        context_ = std::make_unique<plugin::HostServicesContext>(*bridge_, logger_, state_);
        context_->set_phase(plugin::HostServicesContext::Phase::UPDATE);
    }

    coro::Loop loop_;
    std::unique_ptr<plugin::Bridge> bridge_;
    ArmedRecordingLogger logger_;
    std::shared_ptr<plugin::CallState> state_;
    std::unique_ptr<plugin::HostServicesContext> context_;
};

// ===========================================================================
//  Services table
// ===========================================================================

TEST_F(HostServicesTest, MakeServices_BindsThisContextAndTheHostAbiVersion) {
    const auto services = context_->make_services();

    EXPECT_EQ(services.struct_size, sizeof(yaddnsc_host_services));
    EXPECT_EQ(services.abi_major, YADDNSC_DRIVER_ABI_MAJOR);
    EXPECT_EQ(services.abi_minor, YADDNSC_DRIVER_ABI_MINOR);
    EXPECT_EQ(static_cast<const void*>(services.context), static_cast<const void*>(context_.get()));
    EXPECT_NE(services.log, nullptr);
    EXPECT_NE(services.http_exchange, nullptr);
    EXPECT_NE(services.is_cancelled, nullptr);
    EXPECT_EQ(services.is_cancelled(services.context), 0);
}

TEST_F(HostServicesTest, MakeServicesV11_ExposesTheOptionalQuery) {
    const auto services = context_->make_services_v1_1();

    EXPECT_EQ(services.base.struct_size, sizeof(yaddnsc_host_services_v1_1));
    EXPECT_EQ(services.base.abi_major, YADDNSC_DRIVER_ABI_MAJOR);
    EXPECT_EQ(services.base.abi_minor, YADDNSC_DRIVER_ABI_MINOR);
    EXPECT_EQ(static_cast<const void*>(services.base.context), static_cast<const void*>(context_.get()));
    EXPECT_NE(services.base.log, nullptr);
    EXPECT_NE(services.base.http_exchange, nullptr);
    EXPECT_NE(services.base.is_cancelled, nullptr);
    ASSERT_NE(services.log_enabled, nullptr);
    EXPECT_EQ(services.log_enabled(services.base.context, YADDNSC_LOG_INFO), 1);

    logger_.disable_debug = true;
    EXPECT_EQ(services.log_enabled(services.base.context, YADDNSC_LOG_DEBUG), 0);
    EXPECT_EQ(services.log_enabled(nullptr, YADDNSC_LOG_INFO), 0);
}

// ===========================================================================
//  log() — level mapping and the C-frame firewall
// ===========================================================================

TEST_F(HostServicesTest, Log_MapsEveryAbiLevelAndRecordsUnknownLevelsAsInfo) {
    const auto services = context_->make_services();
    const yaddnsc_source_location location{{"driver.cpp", 10}, 42, {"update", 6}};

    struct Case {
        yaddnsc_log_level level;
        app::LogLevel expected;
    };

    const std::array cases{
        Case{YADDNSC_LOG_TRACE, app::LogLevel::TRACE}, Case{YADDNSC_LOG_DEBUG, app::LogLevel::DEBUG},
        Case{YADDNSC_LOG_INFO, app::LogLevel::INFO},   Case{YADDNSC_LOG_WARN, app::LogLevel::WARN},
        Case{YADDNSC_LOG_ERROR, app::LogLevel::ERROR}, Case{YADDNSC_LOG_ERROR + 1, app::LogLevel::INFO},
    };

    for (const auto& one : cases) {
        services.log(services.context, one.level, yaddnsc_string{"plugin message", 14}, &location);
    }

    ASSERT_EQ(logger_.records().size(), cases.size());
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(logger_.records()[i].level, cases[i].expected) << "case " << i;
        EXPECT_EQ(logger_.records()[i].message, "plugin message");
        EXPECT_EQ(logger_.records()[i].file, "driver.cpp");
        EXPECT_EQ(logger_.records()[i].line, 42);
        EXPECT_EQ(logger_.records()[i].function, "update");
    }
}

TEST_F(HostServicesTest, Log_DisabledLevelIsDroppedBeforeLogExplicit) {
    const auto services = context_->make_services();
    logger_.disable_debug = true;

    services.log(services.context, YADDNSC_LOG_DEBUG, yaddnsc_string{"hidden", 6}, nullptr);
    EXPECT_TRUE(logger_.records().empty());

    services.log(services.context, YADDNSC_LOG_INFO, yaddnsc_string{"visible", 7}, nullptr);
    ASSERT_EQ(logger_.records().size(), 1u);
    EXPECT_EQ(logger_.records()[0].level, app::LogLevel::INFO);
    EXPECT_EQ(logger_.records()[0].message, "visible");
}

TEST_F(HostServicesTest, Log_WithoutLocation_StillRecordsTheMessage) {
    const auto services = context_->make_services();

    services.log(services.context, YADDNSC_LOG_INFO, yaddnsc_string{"no location", 11}, nullptr);

    ASSERT_EQ(logger_.records().size(), 1u);
    EXPECT_EQ(logger_.records()[0].level, app::LogLevel::INFO);
    EXPECT_EQ(logger_.records()[0].message, "no location");
    EXPECT_TRUE(logger_.records()[0].file.empty());
    EXPECT_EQ(logger_.records()[0].line, 0);
    EXPECT_TRUE(logger_.records()[0].function.empty());
}

TEST_F(HostServicesTest, Log_LocationWithoutStorage_RecordsEmptyFileAndFunction) {
    const auto services = context_->make_services();
    yaddnsc_source_location location{};
    location.file = {nullptr, 0};
    location.line = 7;
    location.function = {nullptr, 0};

    services.log(services.context, YADDNSC_LOG_WARN, yaddnsc_string{"bare line", 9}, &location);

    ASSERT_EQ(logger_.records().size(), 1u);
    EXPECT_EQ(logger_.records()[0].line, 7);
    EXPECT_TRUE(logger_.records()[0].file.empty());
    EXPECT_TRUE(logger_.records()[0].function.empty());
}

TEST_F(HostServicesTest, Log_LoggerDefect_DoesNotEscapeIntoThePluginFrame) {
    const auto services = context_->make_services();
    const yaddnsc_source_location location{{"driver.cpp", 10}, 42, {"update", 6}};
    logger_.armed = true;

    EXPECT_NO_THROW(services.log(services.context, YADDNSC_LOG_ERROR, yaddnsc_string{"boom", 4}, &location));

    EXPECT_TRUE(logger_.records().empty());
}

// ===========================================================================
//  http_exchange — the unavailable table
// ===========================================================================

TEST_F(HostServicesTest, Exchange_DuringValidatePhase_RejectsWithoutTouchingTheBridge) {
    context_->set_phase(plugin::HostServicesContext::Phase::VALIDATE);
    const auto services = context_->make_services();
    const auto request = valid_request();
    auto response = sized_response();
    auto error = sized_error();

    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);

    EXPECT_EQ(error.status, YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "http_exchange is only available during yaddnsc_driver_update");
    EXPECT_EQ(error.struct_size, sizeof(yaddnsc_error));
    EXPECT_EQ(response.status_code, 0u);
    EXPECT_EQ(response.headers, nullptr);
    EXPECT_EQ(response.body.data, nullptr);
}

// ===========================================================================
//  http_exchange — the null and struct_size boundaries
// ===========================================================================

TEST_F(HostServicesTest, Exchange_RejectsNullContextRequestAndOutResponse) {
    const auto services = context_->make_services();
    const auto request = valid_request();
    constexpr std::string_view message = "context, request and out_response must not be null";

    {  // null context
        auto response = sized_response();
        auto error = sized_error();
        EXPECT_EQ(services.http_exchange(nullptr, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(error.status, YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), message);
    }
    {  // null request
        auto response = sized_response();
        auto error = sized_error();
        EXPECT_EQ(services.http_exchange(services.context, nullptr, &response, &error),
                  YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), message);
    }
    {  // null out_response
        auto error = sized_error();
        EXPECT_EQ(services.http_exchange(services.context, &request, nullptr, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), message);
    }
    {  // null out_error: the returned status stays authoritative, and the
        // error struct is left untouched because there is none.
        state_->cancelled.store(true);
        auto response = sized_response();
        EXPECT_EQ(services.http_exchange(services.context, &request, &response, nullptr), YADDNSC_STATUS_CANCELLED);
        EXPECT_EQ(response.status_code, 0u);
        EXPECT_EQ(response.header_count, 0u);
    }
}

TEST_F(HostServicesTest, Exchange_RejectsUndersizedRequestStruct) {
    const auto services = context_->make_services();
    auto request = valid_request();
    request.struct_size = YADDNSC_HTTP_REQUEST_MIN_SIZE - 1;
    auto response = sized_response();
    auto error = sized_error();

    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "http request struct_size too small");
}

TEST_F(HostServicesTest, Exchange_RejectsUndersizedResponseStruct) {
    const auto services = context_->make_services();
    const auto request = valid_request();
    auto response = sized_response();
    response.struct_size = YADDNSC_HTTP_RESPONSE_MIN_SIZE - 1;
    auto error = sized_error();

    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(message_of(error), "http response struct_size too small");
}

TEST_F(HostServicesTest, Exchange_AcceptsUndersizedErrorStructAndLeavesItUnchanged) {
    state_->cancelled.store(true);
    const auto services = context_->make_services();
    const auto request = valid_request();
    auto response = sized_response();
    yaddnsc_error error{};
    error.struct_size = YADDNSC_ERROR_MIN_SIZE - 1;
    error.status = YADDNSC_STATUS_NETWORK_ERROR;

    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_CANCELLED);

    // Below the ABI 1.0 baseline write_error refuses to write, so the field
    // the caller supplied survives untouched.
    EXPECT_EQ(error.status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(error.struct_size, YADDNSC_ERROR_MIN_SIZE - 1);
    EXPECT_EQ(error.message.data, nullptr);
}

// ===========================================================================
//  http_exchange — request view validation
// ===========================================================================

TEST_F(HostServicesTest, Exchange_RejectsMalformedRequestViews) {
    const auto services = context_->make_services();

    struct Case {
        yaddnsc_http_request request;
        std::string_view message;
    };

    const std::array cases{
        // Empty url, and a url with a length but no storage.
        Case{malformed([](yaddnsc_http_request& r) { r.url = {nullptr, 0}; }), "request url must not be empty"},
        Case{malformed([](yaddnsc_http_request& r) { r.url = {nullptr, 4}; }), "request url must not be empty"},
        // A method constant outside the eight defined verbs.
        Case{malformed([](yaddnsc_http_request& r) { r.method = 0; }), "unknown http method"},
        Case{malformed([](yaddnsc_http_request& r) { r.method = YADDNSC_HTTP_OPTIONS + 1; }), "unknown http method"},
        // A header count without a header array.
        Case{malformed([](yaddnsc_http_request& r) {
                 r.headers = nullptr;
                 r.header_count = 2;
             }),
             "header_count > 0 with null headers"},
        // A content_type view with a length but no storage.
        Case{malformed([](yaddnsc_http_request& r) { r.content_type = {nullptr, 4}; }), "invalid content_type view"},
        // A body size without body storage: {NULL, 0} would be legal.
        Case{malformed([](yaddnsc_http_request& r) { r.body = {nullptr, 3}; }), "body size > 0 with null body data"},
        // A well-formed array whose second entry is not a legal view.
        Case{malformed([](yaddnsc_http_request& r) {
                 r.headers = kInvalidHeaders;
                 r.header_count = std::size(kInvalidHeaders);
             }),
             "invalid HTTP header view"},
    };

    for (const auto& one : cases) {
        auto response = sized_response();
        auto error = sized_error();
        EXPECT_EQ(services.http_exchange(services.context, &one.request, &response, &error),
                  YADDNSC_STATUS_INVALID_ARGUMENT)
            << one.message;
        EXPECT_EQ(error.status, YADDNSC_STATUS_INVALID_ARGUMENT) << one.message;
        EXPECT_EQ(message_of(error), one.message);
        EXPECT_EQ(response.status_code, 0u) << one.message;
    }
}

TEST_F(HostServicesTest, Exchange_AcceptsEveryAbiMethodBeforeValidatingTheRest) {
    const auto services = context_->make_services();
    constexpr std::array methods{YADDNSC_HTTP_GET,   YADDNSC_HTTP_POST, YADDNSC_HTTP_PUT,    YADDNSC_HTTP_DELETE,
                                 YADDNSC_HTTP_PATCH, YADDNSC_HTTP_HEAD, YADDNSC_HTTP_OPTIONS};

    for (const auto method : methods) {
        auto request = valid_request();
        request.method = method;
        request.headers = kValidHeaders;
        request.header_count = std::size(kValidHeaders);
        // An invalid content_type stops the request one check after the verb
        // is mapped, so every defined method is observed without a bridge.
        request.content_type = {nullptr, 4};
        auto response = sized_response();
        auto error = sized_error();

        EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error),
                  YADDNSC_STATUS_INVALID_ARGUMENT)
            << "method " << method;
        EXPECT_EQ(message_of(error), "invalid content_type view") << "method " << method;
    }
}

// ===========================================================================
//  http_exchange — cancellation
// ===========================================================================

TEST_F(HostServicesTest, Exchange_WhenCycleIsCancelled_FailsWithoutPosting) {
    const auto services = context_->make_services();
    state_->cancelled.store(true, std::memory_order_release);
    EXPECT_EQ(services.is_cancelled(services.context), 1);

    const auto request = valid_request();
    auto response = sized_response();
    auto error = sized_error();

    EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error), YADDNSC_STATUS_CANCELLED);

    EXPECT_EQ(error.status, YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(message_of(error), "the request was cancelled");
    EXPECT_EQ(response.status_code, 0u);
    EXPECT_EQ(response.headers, nullptr);
}

// ===========================================================================
//  http_exchange — the exception firewall
// ===========================================================================

TEST_F(HostServicesTest, Exchange_HostException_BecomesInternalErrorAndIsPreserved) {
    const auto services = context_->make_services();
    const auto request = valid_request();
    auto response = sized_response();
    auto error = sized_error();
    yaddnsc_status status;
    {
        const InjectFault fault{Fault::std_exception};
        status = services.http_exchange(services.context, &request, &response, &error);
    }

    EXPECT_EQ(status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(message_of(error), "injected host defect");
    EXPECT_EQ(response.status_code, 0u);
    // The first host defect survives on the cycle for the gateway's boundary.
    EXPECT_TRUE(state_->worker_error);
}

TEST_F(HostServicesTest, Exchange_LongHostExceptionMessage_IsClampedToTheCallbackBuffer) {
    const auto services = context_->make_services();
    const auto request = valid_request();
    auto response = sized_response();
    auto error = sized_error();
    yaddnsc_status status;
    {
        const InjectFault fault{Fault::long_std_exception};
        status = services.http_exchange(services.context, &request, &response, &error);
    }

    EXPECT_EQ(status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(message_of(error).size(), kCallbackBufferCapacity - 1);
    EXPECT_EQ(std::string(message_of(error)), std::string(kCallbackBufferCapacity - 1, 'x'));
    EXPECT_TRUE(state_->worker_error);
}

TEST_F(HostServicesTest, Exchange_CancelledException_BecomesCancelledStatus) {
    const auto services = context_->make_services();
    coro::CancelScope scope;
    scope.cancel();
    cancellation_origin = &scope;

    const auto request = valid_request();
    auto response = sized_response();
    auto error = sized_error();
    yaddnsc_status status;
    {
        const InjectFault fault{Fault::cancelled};
        status = services.http_exchange(services.context, &request, &response, &error);
    }
    cancellation_origin = nullptr;

    EXPECT_EQ(status, YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(error.status, YADDNSC_STATUS_CANCELLED);
    EXPECT_EQ(message_of(error), "the request was cancelled");
    // Cancellation is control flow, not a host defect: it must not be
    // recorded as one.
    EXPECT_FALSE(state_->worker_error);
}

TEST_F(HostServicesTest, Exchange_UnknownException_BecomesGenericInternalError) {
    const auto services = context_->make_services();
    const auto request = valid_request();
    auto response = sized_response();
    auto error = sized_error();
    yaddnsc_status status;
    {
        const InjectFault fault{Fault::foreign_exception};
        status = services.http_exchange(services.context, &request, &response, &error);
    }

    EXPECT_EQ(status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(message_of(error), "unknown host exception during http exchange");
    EXPECT_TRUE(state_->worker_error);
}

TEST_F(HostServicesTest, Exchange_RepeatedHostDefects_PreserveOnlyTheFirstOne) {
    const auto services = context_->make_services();
    const auto request = valid_request();
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto response = sized_response();
        auto error = sized_error();
        {
            const InjectFault fault{Fault::foreign_exception};
            EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error),
                      YADDNSC_STATUS_INTERNAL_ERROR);
        }
    }
    EXPECT_TRUE(state_->worker_error);
}

}  // namespace

TEST_F(HostServicesTest, SavedServicesTableChecksEveryCurrentPhase) {
    const auto services = context_->make_services();
    const auto request = valid_request();
    auto response = sized_response();
    auto error = sized_error();
    for (auto phase : {plugin::HostServicesContext::Phase::CREATE, plugin::HostServicesContext::Phase::VALIDATE,
                       plugin::HostServicesContext::Phase::DESTROY}) {
        context_->set_phase(phase);
        EXPECT_EQ(services.http_exchange(services.context, &request, &response, &error),
                  YADDNSC_STATUS_INVALID_ARGUMENT);
        EXPECT_EQ(message_of(error), "http_exchange is only available during yaddnsc_driver_update");
        EXPECT_FALSE(state_->worker_error);
    }
    context_->set_phase(plugin::HostServicesContext::Phase::UPDATE);
    state_->cancelled.store(true);
    EXPECT_EQ(services.http_exchange(services.context, &request, &response, nullptr), YADDNSC_STATUS_CANCELLED);
}
