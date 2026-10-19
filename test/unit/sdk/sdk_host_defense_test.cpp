//
// Unit tests for the plugin SDK's defence against a malformed host
// (<yaddnsc/sdk/driver.hpp>, class Services).
//
// The host is the other side of the C ABI, so a plugin must not trust what it
// is handed: a table that is null, truncated, ABI-incompatible or missing its
// exchange entry point has to be refused rather than dereferenced, and a host
// that returns an unknown status or an inconsistent error report has to be
// reported as an internal error rather than acted upon.
//
// These are the guards the C ABI round-trip tests cannot reach, because the
// production host is well formed by construction. Each case breaks exactly one
// thing about an otherwise valid table, so a failure names the broken clause.
//
// No socket, no worker, no filesystem: the exchange entry point is a local
// function pointer the test controls.
//

#include <cstdint>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>
#include <yaddnsc/sdk/driver_common.hpp>

namespace {

using yaddnsc::sdk::Driver;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::Services;

namespace {

int g_cancelled_calls = 0;
int g_exchange_calls = 0;

/// The malformed response a case asks the fake host to produce.
struct Script {
    yaddnsc_status status = YADDNSC_STATUS_OK;
    bool write_error_report = false;
    yaddnsc_status error_status = YADDNSC_STATUS_OK;
    std::string_view message{};
    uint32_t retry_after_seconds = 0;
    uint32_t response_status_code = 200;
    std::string_view response_body{"body"};
    bool valid_response_body = true;
};

int32_t is_cancelled(void* context) noexcept {
    ++g_cancelled_calls;
    return context != nullptr ? 1 : 0;
}

/// Writes whatever the case under test asks for, so the caller's post-checks
/// are what decide the outcome.
yaddnsc_status http_exchange(void* context, const yaddnsc_http_request* /*request*/,
                              yaddnsc_http_response* out_response, yaddnsc_error* out_error) noexcept {
    ++g_exchange_calls;
    const auto* script = static_cast<const Script*>(context);
    if (script->status != YADDNSC_STATUS_OK) {
        if (script->write_error_report && out_error != nullptr &&
            out_error->struct_size >= YADDNSC_ERROR_MIN_SIZE) {
            out_error->status = script->error_status;
            out_error->retry_after_seconds = script->retry_after_seconds;
            const std::string_view message = script->message;
            out_error->message = {message.data(), message.size()};
        }
        return script->status;
    }
    if (out_response != nullptr) {
        out_response->status_code = script->response_status_code;
        if (script->valid_response_body) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(script->response_body.data());
            out_response->body.data = bytes;
            out_response->body.size = script->response_body.size();
        } else {
            // A null data pointer with a non-zero size is the classic
            // "has_valid_response" trap.
            out_response->body.data = nullptr;
            out_response->body.size = 16;
        }
    }
    return YADDNSC_STATUS_OK;
}

/// A fully valid services table, then broken by one field per test.
[[nodiscard]] yaddnsc_host_services valid_table(void* context) {
    yaddnsc_host_services services{};
    services.struct_size = static_cast<uint32_t>(sizeof(yaddnsc_host_services));
    services.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    services.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    services.context = context;
    services.log = nullptr;
    services.http_exchange = &http_exchange;
    services.is_cancelled = &is_cancelled;
    return services;
}

[[nodiscard]] HttpRequest a_request() {
    HttpRequest request;
    request.url = "https://example.test/";
    return request;
}

}  // namespace

/// Every rejection of the table itself carries this one message: the plugin
/// cannot tell the host which clause was wrong, only that it must not proceed.
void expect_table_refused(const Services& services) {
    const auto result = services.exchange(a_request());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string_view{result.error().message}, "invalid host services table");
}

}  // namespace

// ---------------------------------------------------------------------------
// Table validation — nothing below this line may dereference the table
// ---------------------------------------------------------------------------

TEST(SdkHostDefence, NullServicesTable_IsRefusedWithoutDereferencing) {
    expect_table_refused(Services{nullptr});
}

TEST(SdkHostDefence, TruncatedBeforeTheAbiPrefix_IsRefused) {
    Script script;
    auto services = valid_table(&script);
    services.struct_size = YADDNSC_ABI_VERSION_PREFIX_SIZE - 1;

    expect_table_refused(Services{&services});
}

TEST(SdkHostDefence, AbiMajorTheDriverCannotSpeak_IsRefused) {
    Script script;
    auto services = valid_table(&script);
    services.abi_major = YADDNSC_DRIVER_ABI_MAJOR + 1;

    expect_table_refused(Services{&services});
}

TEST(SdkHostDefence, TruncatedBeforeTheMinimumTableSize_IsRefused) {
    Script script;
    auto services = valid_table(&script);
    // Large enough for the ABI prefix, too small for the fields the driver
    // reads — the two clauses must not be mistaken for each other.
    services.struct_size = YADDNSC_ABI_VERSION_PREFIX_SIZE;

    expect_table_refused(Services{&services});
}

TEST(SdkHostDefence, NullContext_IsRefused) {
    Script script;
    auto services = valid_table(&script);
    services.context = nullptr;

    expect_table_refused(Services{&services});
}

TEST(SdkHostDefence, MissingExchangeEntryPoint_IsRefused) {
    Script script;
    auto services = valid_table(&script);
    services.http_exchange = nullptr;

    expect_table_refused(Services{&services});
}

TEST(SdkHostDefence, RefusedTable_NeverReachesTheHost) {
    Script script;
    auto services = valid_table(&script);
    services.http_exchange = nullptr;
    const int before = g_exchange_calls;

    expect_table_refused(Services{&services});

    EXPECT_EQ(g_exchange_calls, before) << "a refused table must not call the host";
}

// ---------------------------------------------------------------------------
// What the host returns — the report itself has to be trustworthy too
// ---------------------------------------------------------------------------

TEST(SdkHostDefence, HostReturningAnUnknownStatus_IsAnInternalError) {
    Script script;
    script.status = YADDNSC_STATUS_OK + 4242;  // outside the closed set
    auto services = valid_table(&script);

    const auto result = Services{&services}.exchange(a_request());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string_view{result.error().message}, "host returned an unknown status");
}

TEST(SdkHostDefence, FailureWithoutAnErrorReport_IsAnInternalError) {
    // A non-OK status with out_error left untouched: the driver cannot report
    // a failure it has no message for, so it must not invent one.
    Script script;
    script.status = YADDNSC_STATUS_AUTHENTICATION_FAILED;
    script.write_error_report = false;
    auto services = valid_table(&script);

    const auto result = Services{&services}.exchange(a_request());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string_view{result.error().message}, "host returned an invalid error report");
}

TEST(SdkHostDefence, ErrorReportDisagreeingWithTheStatus_IsAnInternalError) {
    Script script;
    script.status = YADDNSC_STATUS_AUTHENTICATION_FAILED;
    script.write_error_report = true;
    script.error_status = YADDNSC_STATUS_NETWORK_ERROR;  // disagrees
    script.message = "host said one thing and returned another";
    auto services = valid_table(&script);

    const auto result = Services{&services}.exchange(a_request());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string_view{result.error().message}, "host returned an invalid error report");
}

TEST(SdkHostDefence, ConsistentErrorReport_IsPassedThroughWithItsDelay) {
    Script script;
    script.status = YADDNSC_STATUS_RATE_LIMITED;
    script.write_error_report = true;
    script.error_status = YADDNSC_STATUS_RATE_LIMITED;
    script.message = "slow down";
    script.retry_after_seconds = 30;
    auto services = valid_table(&script);

    const auto result = Services{&services}.exchange(a_request());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_RATE_LIMITED);
    EXPECT_EQ(std::string_view{result.error().message}, "slow down");
    EXPECT_EQ(result.error().retry_after_seconds, 30U);
}

TEST(SdkHostDefence, ResponseWithANullBodyPointer_IsAnInvalidResponse) {
    // struct_size is right, so only the body's own consistency can reject it.
    Script script;
    script.valid_response_body = false;
    auto services = valid_table(&script);

    const auto result = Services{&services}.exchange(a_request());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string_view{result.error().message}, "host returned an invalid HTTP response");
}

TEST(SdkHostDefence, ValidResponseIsReturned) {
    Script script;
    script.response_status_code = 200;
    script.response_body = "{\"ok\":true}";
    auto services = valid_table(&script);

    const auto result = Services{&services}.exchange(a_request());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 200U);
    EXPECT_EQ(std::string_view{result->body}, "{\"ok\":true}");
}

// ---------------------------------------------------------------------------
// is_cancelled — a null table is "not cancelled", never a crash
// ---------------------------------------------------------------------------

TEST(SdkHostDefence, IsCancelled_OnANullTableOrMissingCallback_IsFalse) {
    const int before = g_cancelled_calls;
    EXPECT_FALSE(Services{nullptr}.is_cancelled());

    Script script;
    auto services = valid_table(&script);
    services.is_cancelled = nullptr;
    EXPECT_FALSE(Services{&services}.is_cancelled());
    EXPECT_EQ(g_cancelled_calls, before) << "a table without a cancellation callback must not reach the host";
}

TEST(SdkHostDefence, IsCancelled_ForwardsTheHostVerdict) {
    Script script;
    auto services = valid_table(&script);
    services.context = &script;  // non-null context reads as cancelled
    const int before = g_cancelled_calls;

    EXPECT_TRUE(Services{&services}.is_cancelled());
    EXPECT_EQ(g_cancelled_calls, before + 1) << "the verdict must come from the host callback, not from the driver";
}
