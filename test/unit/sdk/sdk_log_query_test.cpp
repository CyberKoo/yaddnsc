//
// Unit tests for the ABI 1.1 optional log-level query:
//   - copy_services_v1_1 gates the tail on version, size and field coverage;
//   - Services/UpdateContext carry the query only on the SDK factory path;
//   - YADDNSC_SDK_LOG evaluates the level, queries it, then skips argument
//     evaluation and formatting when disabled; without a log callback neither
//     the level nor the arguments are evaluated;
//   - fallback without a query and on a throwing query.
//
// The host table is a real yaddnsc_host_services_v1_1 container, so the
// positive cases exercise the same first-member layout the production host
// uses. The negative cases mutate only the version/size gates; they never
// point the SDK at a 40-byte table that lies about its size.
// =============================================================================

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/driver_abi.h>

namespace {

int g_argument_evaluations = 0;

int evaluate_argument() {
    ++g_argument_evaluations;
    return 7;
}

struct QueryHost {
    yaddnsc_host_services_v1_1 table{};
    int query_calls = 0;
    int log_calls = 0;
    yaddnsc_log_level last_level = YADDNSC_LOG_INFO;
    bool enabled = true;

    QueryHost() {
        table.base.struct_size = static_cast<uint32_t>(sizeof(yaddnsc_host_services_v1_1));
        table.base.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
        table.base.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
        table.base.context = this;
        table.base.log = &log_entry;
        table.base.http_exchange = &exchange_entry;
        table.base.is_cancelled = &is_cancelled_entry;
        table.log_enabled = &query_entry;
    }

    static void log_entry(void* context, yaddnsc_log_level, yaddnsc_string, const yaddnsc_source_location*) {
        ++static_cast<QueryHost*>(context)->log_calls;
    }

    static int32_t query_entry(void* context, yaddnsc_log_level level) {
        auto& self = *static_cast<QueryHost*>(context);
        ++self.query_calls;
        self.last_level = level;
        return self.enabled ? 1 : 0;
    }

    static yaddnsc_status exchange_entry(void*, const yaddnsc_http_request*,
                                         yaddnsc_http_response* out_response, yaddnsc_error*) {
        out_response->status_code = 204;
        out_response->headers = nullptr;
        out_response->header_count = 0;
        out_response->body = {};
        return YADDNSC_STATUS_OK;
    }

    static int32_t is_cancelled_entry(void*) { return 0; }
};

int32_t throwing_query(void*, yaddnsc_log_level) {
    throw std::runtime_error("query failed");
}

[[nodiscard]] yaddnsc_update_request blank_request() {
    yaddnsc_update_request request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    return request;
}

class WarnDriver final : public yaddnsc::sdk::Driver {
public:
    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view) const override { return {}; }

    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext& context) override {
        return run_update(context, "warn-driver", yaddnsc::sdk::HttpRequest{},
                          [](const yaddnsc::sdk::HttpResponse&, const yaddnsc::sdk::Services&) { return false; });
    }
};

class QueryAwareDriver final : public yaddnsc::sdk::Driver {
public:
    explicit QueryAwareDriver(yaddnsc::sdk::Services services) {
        YADDNSC_SDK_LOG_DEBUG(services, "{}", evaluate_argument());
    }

    [[nodiscard]] yaddnsc::sdk::Result validate(std::string_view) const override { return {}; }

    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext& context) override {
        YADDNSC_SDK_LOG_DEBUG(context, "{}", evaluate_argument());
        return {};
    }
};

class SdkLogQueryTest : public ::testing::Test {
protected:
    void SetUp() override { g_argument_evaluations = 0; }
};

}  // namespace

TEST_F(SdkLogQueryTest, CopyServicesV11GatesTheTailOnVersionAndSize) {
    QueryHost host;

    const auto complete = yaddnsc::sdk::detail::copy_services_v1_1(host.table.base);
    EXPECT_EQ(complete.base.struct_size, sizeof(yaddnsc_host_services_v1_1));
    EXPECT_EQ(complete.log_enabled, &QueryHost::query_entry);

    host.table.base.struct_size = YADDNSC_HOST_SERVICES_MIN_SIZE;
    const auto base_only = yaddnsc::sdk::detail::copy_services_v1_1(host.table.base);
    EXPECT_EQ(base_only.base.struct_size, YADDNSC_HOST_SERVICES_MIN_SIZE);
    EXPECT_EQ(base_only.log_enabled, nullptr);

    host.table.base.struct_size = YADDNSC_HOST_SERVICES_V1_1_MIN_SIZE - 1;
    const auto partial_tail = yaddnsc::sdk::detail::copy_services_v1_1(host.table.base);
    EXPECT_EQ(partial_tail.base.struct_size, YADDNSC_HOST_SERVICES_MIN_SIZE);
    EXPECT_EQ(partial_tail.log_enabled, nullptr);

    host.table.base.struct_size = sizeof(yaddnsc_host_services_v1_1);
    host.table.base.abi_minor = 0;
    const auto old_minor = yaddnsc::sdk::detail::copy_services_v1_1(host.table.base);
    EXPECT_EQ(old_minor.base.struct_size, YADDNSC_HOST_SERVICES_MIN_SIZE);
    EXPECT_EQ(old_minor.log_enabled, nullptr);

    host.table.base.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    host.table.log_enabled = nullptr;
    const auto null_query = yaddnsc::sdk::detail::copy_services_v1_1(host.table.base);
    EXPECT_EQ(null_query.base.struct_size, sizeof(yaddnsc_host_services_v1_1));
    EXPECT_EQ(null_query.log_enabled, nullptr);

    host.table.log_enabled = &QueryHost::query_entry;
    host.table.base.struct_size = sizeof(yaddnsc_host_services_v1_1) + 64;
    const auto larger = yaddnsc::sdk::detail::copy_services_v1_1(host.table.base);
    EXPECT_EQ(larger.base.struct_size, sizeof(yaddnsc_host_services_v1_1));
    EXPECT_EQ(larger.log_enabled, &QueryHost::query_entry);
}

TEST_F(SdkLogQueryTest, MacroQueriesBeforeEvaluatingArguments) {
    QueryHost host;
    yaddnsc::sdk::Services services{&host.table.base, host.table.log_enabled};
    int level_evaluations = 0;
    const auto level = [&] {
        ++level_evaluations;
        return YADDNSC_LOG_INFO;
    };

    host.enabled = false;
    YADDNSC_SDK_LOG(services, level(), "{}", evaluate_argument());
    EXPECT_EQ(host.query_calls, 1);
    EXPECT_EQ(level_evaluations, 1);
    EXPECT_EQ(g_argument_evaluations, 0);
    EXPECT_EQ(host.log_calls, 0);
    EXPECT_EQ(host.last_level, YADDNSC_LOG_INFO);

    host.enabled = true;
    YADDNSC_SDK_LOG(services, level(), "{}", evaluate_argument());
    EXPECT_EQ(host.query_calls, 2);
    EXPECT_EQ(level_evaluations, 2);
    EXPECT_EQ(g_argument_evaluations, 1);
    EXPECT_EQ(host.log_calls, 1);
}

TEST_F(SdkLogQueryTest, MacroFallsBackWithoutAQuery) {
    QueryHost host;
    yaddnsc::sdk::Services services{&host.table.base};
    host.enabled = false;

    YADDNSC_SDK_LOG_INFO(services, "{}", evaluate_argument());
    EXPECT_EQ(host.query_calls, 0);
    EXPECT_EQ(g_argument_evaluations, 1);
    EXPECT_EQ(host.log_calls, 1);
}

TEST_F(SdkLogQueryTest, MacroSkipsLevelAndArgumentsWithoutALogCallback) {
    QueryHost host;
    host.table.base.log = nullptr;
    yaddnsc::sdk::Services services{&host.table.base, host.table.log_enabled};
    int level_evaluations = 0;

    YADDNSC_SDK_LOG(services, (++level_evaluations, YADDNSC_LOG_INFO), "{}", evaluate_argument());
    EXPECT_EQ(host.query_calls, 0);
    EXPECT_EQ(level_evaluations, 0);
    EXPECT_EQ(g_argument_evaluations, 0);
}

TEST_F(SdkLogQueryTest, MacroFallsBackWhenTheQueryThrows) {
    QueryHost host;
    host.table.log_enabled = &throwing_query;
    yaddnsc::sdk::Services services{&host.table.base, host.table.log_enabled};

    EXPECT_NO_THROW(YADDNSC_SDK_LOG_INFO(services, "{}", evaluate_argument()));
    EXPECT_EQ(g_argument_evaluations, 1);
    EXPECT_EQ(host.log_calls, 1);
}

TEST_F(SdkLogQueryTest, FactoryCarriesTheQueryIntoInjectedServices) {
    QueryHost host;
    host.enabled = false;
    yaddnsc_driver* driver = nullptr;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    ASSERT_EQ((yaddnsc::sdk::detail::create_driver<QueryAwareDriver>(&host.table.base, &driver, &error)),
              YADDNSC_STATUS_OK);
    ASSERT_NE(driver, nullptr);
    EXPECT_EQ(host.query_calls, 1);
    EXPECT_EQ(g_argument_evaluations, 0);
    EXPECT_EQ(host.log_calls, 0);
    yaddnsc::sdk::detail::destroy_driver(driver);

    host.enabled = true;
    driver = nullptr;
    ASSERT_EQ((yaddnsc::sdk::detail::create_driver<QueryAwareDriver>(&host.table.base, &driver, &error)),
              YADDNSC_STATUS_OK);
    EXPECT_EQ(host.query_calls, 2);
    EXPECT_EQ(g_argument_evaluations, 1);
    EXPECT_EQ(host.log_calls, 1);
    yaddnsc::sdk::detail::destroy_driver(driver);
}

TEST_F(SdkLogQueryTest, UpdateContextCarriesTheQuery) {
    QueryHost host;
    host.enabled = false;
    yaddnsc_driver* driver = nullptr;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    ASSERT_EQ((yaddnsc::sdk::detail::create_driver<QueryAwareDriver>(&host.table.base, &driver, &error)),
              YADDNSC_STATUS_OK);
    host.query_calls = 0;

    const auto request = blank_request();
    EXPECT_EQ(yaddnsc::sdk::detail::update_driver(driver, &request, &error), YADDNSC_STATUS_OK);
    EXPECT_EQ(host.query_calls, 1);
    EXPECT_EQ(g_argument_evaluations, 0);
    EXPECT_EQ(host.log_calls, 0);
    EXPECT_EQ(host.last_level, YADDNSC_LOG_DEBUG);
    yaddnsc::sdk::detail::destroy_driver(driver);
}

TEST_F(SdkLogQueryTest, RunUpdateWarnPathHonoursTheQuery) {
    QueryHost host;
    host.enabled = false;
    yaddnsc_driver* driver = nullptr;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    ASSERT_EQ((yaddnsc::sdk::detail::create_driver<WarnDriver>(&host.table.base, &driver, &error)),
              YADDNSC_STATUS_OK);
    ASSERT_NE(driver, nullptr);

    const auto request = blank_request();
    EXPECT_EQ(yaddnsc::sdk::detail::update_driver(driver, &request, &error),
              YADDNSC_STATUS_UPSTREAM_REJECTED);
    EXPECT_EQ(host.query_calls, 2);
    EXPECT_EQ(host.last_level, YADDNSC_LOG_WARN);
    EXPECT_EQ(host.log_calls, 0);
    yaddnsc::sdk::detail::destroy_driver(driver);

    host.enabled = true;
    driver = nullptr;
    ASSERT_EQ((yaddnsc::sdk::detail::create_driver<WarnDriver>(&host.table.base, &driver, &error)),
              YADDNSC_STATUS_OK);
    EXPECT_EQ(yaddnsc::sdk::detail::update_driver(driver, &request, &error),
              YADDNSC_STATUS_UPSTREAM_REJECTED);
    EXPECT_EQ(host.log_calls, 2);
    yaddnsc::sdk::detail::destroy_driver(driver);
}
