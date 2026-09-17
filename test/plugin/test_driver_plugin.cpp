//
// Created by Kotarou on 2026/9/17.
//

/// Whiteboard contract-test plugin for the v1 alpha ABI.
///
/// Behaviour is steered entirely through driver_param_json so the host-side
/// contract tests can exercise every documented guarantee of the v1 alpha
/// plugin ABI against a real dlopen'ed module:
///
///   {"op":"success"}                          — return OK
///   {"op":"fail","status":<name>}             — return the named status with
///     ["message"][,"retry_after":N]             the given message/retry_after
///   {"op":"exchange","http_count":N}          — perform N exchanges and verify
///                                               earlier response views survive
///   {"op":"log_macro","message":"..."}        — one YADDNSC_SDK_LOG_INFO call
///   {"op":"log_raw"}                          — hand-written log calls with
///                                               missing/broken locations
///   {"op":"check_cancel","expect_cancelled":B}— assert is_cancelled() == B
///   {"op":"echo_params"}                      — log the received update fields
///
/// The extra test_plugin_* exports (not part of the ABI) let tests inject
/// create() failures and observe create/update/destroy ordering.

#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yaddnsc/sdk/driver.hpp>

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::UpdateContext;

namespace {

/// Control protocol; every key is optional and defaults to the no-op case.
struct TestDriverConfig {
    std::optional<std::string> op;
    std::optional<std::string> status;
    std::optional<std::string> message;
    std::optional<uint32_t> retry_after;
    std::optional<uint32_t> http_count;
    std::optional<std::string> url;
    std::optional<bool> expect_cancelled;
};

std::atomic<int> g_create_failures{0};

std::atomic<uint64_t> g_clock{0};
std::atomic<uint64_t> g_creates{0};
std::atomic<uint64_t> g_updates{0};
std::atomic<uint64_t> g_destroys{0};
std::atomic<uint64_t> g_last_create_seq{0};
std::atomic<uint64_t> g_last_update_seq{0};
std::atomic<uint64_t> g_last_destroy_seq{0};

void tick(std::atomic<uint64_t> &slot) {
    const uint64_t seq = g_clock.fetch_add(1, std::memory_order_relaxed) + 1;
    slot.store(seq, std::memory_order_relaxed);
}

[[nodiscard]] yaddnsc_status status_by_name(std::string_view name) {
    if (name == "invalid_argument") {
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    if (name == "invalid_config") {
        return YADDNSC_STATUS_INVALID_CONFIG;
    }
    if (name == "unsupported_record") {
        return YADDNSC_STATUS_UNSUPPORTED_RECORD;
    }
    if (name == "network_error") {
        return YADDNSC_STATUS_NETWORK_ERROR;
    }
    if (name == "authentication_failed") {
        return YADDNSC_STATUS_AUTHENTICATION_FAILED;
    }
    if (name == "rate_limited") {
        return YADDNSC_STATUS_RATE_LIMITED;
    }
    if (name == "upstream_rejected") {
        return YADDNSC_STATUS_UPSTREAM_REJECTED;
    }
    if (name == "invalid_response") {
        return YADDNSC_STATUS_INVALID_RESPONSE;
    }
    if (name == "cancelled") {
        return YADDNSC_STATUS_CANCELLED;
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

} // namespace

template<>
struct glz::meta<TestDriverConfig> {
    using T = TestDriverConfig;
    static constexpr auto value = object(
            "op", &T::op,
            "status", &T::status,
            "message", &T::message,
            "retry_after", &T::retry_after,
            "http_count", &T::http_count,
            "url", &T::url,
            "expect_cancelled", &T::expect_cancelled
    );
};

class TestDriver final : public yaddnsc::sdk::Driver {
public:
    TestDriver() {
        // Injected create failure: while the counter is positive every
        // construction throws with a ticket-unique message, so concurrent
        // create() failures can be checked for message isolation.
        if (const int ticket = g_create_failures.fetch_sub(1, std::memory_order_relaxed); ticket > 0) {
            throw std::runtime_error(fmt::format("injected create failure #{}", ticket));
        }
        g_create_failures.fetch_add(1, std::memory_order_relaxed);

        g_creates.fetch_add(1, std::memory_order_relaxed);
        tick(g_last_create_seq);
    }

    ~TestDriver() override {
        g_destroys.fetch_add(1, std::memory_order_relaxed);
        tick(g_last_destroy_seq);
    }

    Result update(UpdateContext &context) override {
        g_updates.fetch_add(1, std::memory_order_relaxed);
        tick(g_last_update_seq);

        const auto config = parse_config<TestDriverConfig>(context.request().driver_param_json);
        const std::string op = config.op.value_or("success");

        if (op == "success") {
            return {};
        }
        if (op == "fail") {
            return std::unexpected(Error{status_by_name(config.status.value_or("internal_error")),
                                         config.message.value_or(""), config.retry_after.value_or(0)});
        }
        if (op == "exchange") {
            return run_exchanges(context, config);
        }
        if (op == "log_macro") {
            YADDNSC_SDK_LOG_INFO(context, "{}", config.message.value_or("marker"));
            return {};
        }
        if (op == "log_raw") {
            return run_raw_logs(context);
        }
        if (op == "check_cancel") {
            if (context.is_cancelled() != config.expect_cancelled.value_or(false)) {
                return std::unexpected(Error{YADDNSC_STATUS_INTERNAL_ERROR, "cancellation state mismatch", 0});
            }
            return {};
        }
        if (op == "echo_params") {
            const auto &params = context.request();
            YADDNSC_SDK_LOG_INFO(context, "params ip={} rd={} domain={} sub={} fqdn={} param={}", params.ip_address,
                                 params.record_type, params.domain, params.subdomain, params.fqdn,
                                 params.driver_param_json);
            return {};
        }

        return std::unexpected(Error{YADDNSC_STATUS_INTERNAL_ERROR, fmt::format("unknown op: {}", op), 0});
    }

private:
    /// Perform `http_count` exchanges; after each one, every previously
    /// received response view must still byte-compare equal to its snapshot
    /// (the ABI contract: views live until yaddnsc_driver_update() returns).
    static Result run_exchanges(UpdateContext &context, const TestDriverConfig &config) {
        struct Snapshot {
            uint32_t status_code;
            std::string_view body_view;
            std::string body_copy;
            std::string_view header_view;
            std::string header_copy;
        };

        const uint32_t count = config.http_count.value_or(1);
        const std::string url = config.url.value_or("http://localhost/contract");
        std::vector<Snapshot> snapshots;
        snapshots.reserve(count);

        for (uint32_t i = 0; i < count; ++i) {
            HttpRequest request{};
            request.method = yaddnsc::sdk::Method::Post;
            request.url = fmt::format("{}/{}", url, i);
            request.headers.push_back({"X-Seq", fmt::format("{}", i)});
            request.body = fmt::format("plugin-request-{}", i);
            request.content_type = "application/json";

            auto response = context.exchange(request);
            if (!response) {
                return std::unexpected(
                        Error{response.error().status, fmt::format("exchange failed: {}", response.error().message),
                              response.error().retry_after_seconds});
            }

            Snapshot snapshot{};
            snapshot.status_code = response->status_code;
            snapshot.body_view = response->body;
            snapshot.body_copy = std::string(response->body);
            if (!response->headers.empty()) {
                snapshot.header_view = response->headers.front().value;
                snapshot.header_copy = std::string(response->headers.front().value);
            }
            snapshots.push_back(std::move(snapshot));

            // Re-verify every earlier view after this exchange ran.
            for (uint32_t j = 0; j <= i; ++j) {
                if (snapshots[j].body_view != snapshots[j].body_copy ||
                    snapshots[j].header_view != snapshots[j].header_copy) {
                    return std::unexpected(Error{YADDNSC_STATUS_INTERNAL_ERROR,
                                                 fmt::format("response view #{} invalidated by exchange #{}", j, i),
                                                 0});
                }
            }
        }
        return {};
    }

    /// Hand-written log calls that violate the location contract; the host
    /// must tolerate all of them without failing the update.
    static Result run_raw_logs(UpdateContext &context) {
        const auto *services = context.services().get();
        const yaddnsc_string message{"raw log record", sizeof("raw log record") - 1};

        // (a) no location at all
        services->log(services->context, YADDNSC_LOG_INFO, message, nullptr);
        // (b) empty file view
        const yaddnsc_source_location no_file{{nullptr, 0}, 7, {"func", 4}};
        services->log(services->context, YADDNSC_LOG_WARN, message, &no_file);
        // (c) line == 0
        const yaddnsc_source_location zero_line{{"file.cpp", 8}, 0, {"func", 4}};
        services->log(services->context, YADDNSC_LOG_ERROR, message, &zero_line);
        // (d) negative line
        const yaddnsc_source_location negative_line{{"file.cpp", 8}, -3, {"func", 4}};
        services->log(services->context, YADDNSC_LOG_DEBUG, message, &negative_line);
        // (e) empty function view
        const yaddnsc_source_location no_function{{"file.cpp", 8}, 11, {nullptr, 0}};
        services->log(services->context, YADDNSC_LOG_TRACE, message, &no_function);
        // (f) fully valid hand-written call
        const yaddnsc_source_location valid{{"file.cpp", 8}, 13, {"func", 4}};
        services->log(services->context, YADDNSC_LOG_INFO, message, &valid);

        return {};
    }
};

YADDNSC_DEFINE_DRIVER(TestDriver, "test_driver_plugin", "Contract-test whiteboard driver", "yaddnsc", "0.0.0",
                      YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

/* ── Test control exports (not part of the driver ABI) ────────────────────*/

extern "C" YADDNSC_SDK_EXPORT void test_plugin_set_create_failures(int count) {
    g_create_failures.store(count, std::memory_order_relaxed);
}

extern "C" YADDNSC_SDK_EXPORT void test_plugin_reset_state() {
    g_create_failures.store(0, std::memory_order_relaxed);
    g_clock.store(0, std::memory_order_relaxed);
    g_creates.store(0, std::memory_order_relaxed);
    g_updates.store(0, std::memory_order_relaxed);
    g_destroys.store(0, std::memory_order_relaxed);
    g_last_create_seq.store(0, std::memory_order_relaxed);
    g_last_update_seq.store(0, std::memory_order_relaxed);
    g_last_destroy_seq.store(0, std::memory_order_relaxed);
}

extern "C" YADDNSC_SDK_EXPORT void test_plugin_get_state(uint64_t *creates, uint64_t *updates, uint64_t *destroys,
                                                         uint64_t *create_seq, uint64_t *update_seq,
                                                         uint64_t *destroy_seq) {
    *creates = g_creates.load(std::memory_order_relaxed);
    *updates = g_updates.load(std::memory_order_relaxed);
    *destroys = g_destroys.load(std::memory_order_relaxed);
    *create_seq = g_last_create_seq.load(std::memory_order_relaxed);
    *update_seq = g_last_update_seq.load(std::memory_order_relaxed);
    *destroy_seq = g_last_destroy_seq.load(std::memory_order_relaxed);
}
