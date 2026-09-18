//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_TEST_PLUGIN_PLUGIN_TEST_DOUBLES_H
#define YADDNSC_TEST_PLUGIN_PLUGIN_TEST_DOUBLES_H

/// Shared test doubles for the v1 alpha plugin contract tests.
///
/// QueueHttpClient is a scripted HttpClient (the gateway/host-services HTTP
/// boundary); RecordingLogger captures log records with their explicit source
/// location. run_module_cycle() drives one create → update → destroy cycle
/// through a dlopen'ed PluginModule, copying the error report out
/// synchronously exactly as AbiDriverGateway does.

#include <cstdint>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "application/ports/log.h"
#include "infrastructure/plugin/host_services.h"
#include "infrastructure/plugin/plugin_loader.h"
#include "infrastructure/network/http/client_port.h"
#include "support/util/cancellation_token.hpp"

#include <yaddnsc/sdk/driver_abi.h>

/// Scripted HttpClient: replays queued responses/transport errors and keeps
/// an owned copy of every captured request. Thread-safe.
class QueueHttpClient final : public HttpClient {
public:
    struct CapturedRequest {
        std::string url;
        net::http::Method method;
        std::multimap<std::string, std::string> headers;
        std::optional<std::string> body; ///< nullopt = no body at all
        std::string content_type;
    };

    void queue_response(int status_code, std::string body,
                        std::multimap<std::string, std::string> headers = {}) {
        std::lock_guard lock(mutex_);
        queue_.emplace_back(
                std::expected<net::http::Response, net::http::Error>(
                        std::in_place, status_code, std::move(body), std::move(headers)));
    }

    void queue_error(net::http::ErrorCode code, std::string message) {
        std::lock_guard lock(mutex_);
        queue_.emplace_back(std::expected<net::http::Response, net::http::Error>(
                std::unexpect, code, std::move(message)));
    }

    [[nodiscard]] std::expected<net::http::Response, net::http::Error>
    exchange(std::string_view url, const net::http::Request &req,
             const Utils::CancellationToken &) const override {
        std::lock_guard lock(mutex_);
        requests_.push_back(CapturedRequest{std::string(url), req.method, req.headers, req.body, req.content_type});
        if (queue_.empty()) {
            return std::unexpected(net::http::Error{net::http::ErrorCode::CONNECTION_LOST,
                                                    "QueueHttpClient: no queued exchange"});
        }
        auto front = std::move(queue_.front());
        queue_.pop_front();
        return front;
    }

    /// Owned snapshot of every captured request, in arrival order.
    [[nodiscard]] std::vector<CapturedRequest> requests() const {
        std::lock_guard lock(mutex_);
        return requests_;
    }

    [[nodiscard]] size_t request_count() const {
        std::lock_guard lock(mutex_);
        return requests_.size();
    }

    [[nodiscard]] size_t remaining() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    mutable std::deque<std::expected<net::http::Response, net::http::Error>> queue_;
    mutable std::vector<CapturedRequest> requests_;
};

/// Non-owning adapter so an HttpClientFactory can hand out a fresh
/// unique_ptr per update while all of them share one scripted queue.
class SharedHttpClient final : public HttpClient {
public:
    explicit SharedHttpClient(std::shared_ptr<QueueHttpClient> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] std::expected<net::http::Response, net::http::Error>
    exchange(std::string_view url, const net::http::Request &req,
             const Utils::CancellationToken &token) const override {
        return inner_->exchange(url, req, token);
    }

private:
    std::shared_ptr<QueueHttpClient> inner_;
};

/// Logger double: records every record with its (explicit) source location.
/// Thread-safe; every level is enabled.
class RecordingLogger final : public Logger {
public:
    struct Record {
        LogLevel level;
        std::string message;
        std::string file;
        int line;
        std::string function;
    };

    [[nodiscard]] bool is_enabled(LogLevel) const override { return true; }

    void log(LogLevel level, std::string_view message, const std::source_location &loc) const override {
        std::lock_guard lock(mutex_);
        records_.push_back(Record{level, std::string(message), loc.file_name(),
                                  static_cast<int>(loc.line()), loc.function_name()});
    }

    void log_explicit(LogLevel level, std::string_view message, std::string_view file, int line,
                      std::string_view function) const override {
        std::lock_guard lock(mutex_);
        records_.push_back(Record{level, std::string(message), std::string(file), line, std::string(function)});
    }

    [[nodiscard]] std::vector<Record> records() const {
        std::lock_guard lock(mutex_);
        return records_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::vector<Record> records_;
};

/// One host-services context plus its table — the per-update state the
/// gateway builds on the stack, made reusable for tests.
struct HostUpdateContext {
    QueueHttpClient client;
    RecordingLogger logger;
    Utils::CancellationToken token;
    HostServicesContext context;

    HostUpdateContext() : context(client, logger, token) {}

    [[nodiscard]] yaddnsc_host_services services() noexcept { return context.make_services(); }
};

/// Build a fully-populated update request (all views borrow the arguments).
[[nodiscard]] inline yaddnsc_update_request
make_update_request(std::string_view ip_addr, std::string_view rd_type, std::string_view domain,
                    std::string_view subdomain, std::string_view fqdn, std::string_view driver_param_json) {
    return yaddnsc_update_request{
            .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_update_request)),
            .ip_address = {ip_addr.data(), ip_addr.size()},
            .record_type = {rd_type.data(), rd_type.size()},
            .domain = {domain.data(), domain.size()},
            .subdomain = {subdomain.data(), subdomain.size()},
            .fqdn = {fqdn.data(), fqdn.size()},
            .driver_param_json = {reinterpret_cast<const uint8_t *>(driver_param_json.data()),
                                  driver_param_json.size()},
    };
}

/// The outcome of one create → update → destroy cycle through a PluginModule.
struct ModuleCycleResult {
    yaddnsc_status create_status = YADDNSC_STATUS_OK;
    yaddnsc_status update_status = YADDNSC_STATUS_OK;
    std::string error_message;
    uint32_t retry_after_seconds = 0;
};

/// Drive one full update cycle through a module's C entry points, copying
/// the error report out synchronously before destroy (mirrors
/// AbiDriverGateway::update).
[[nodiscard]] inline ModuleCycleResult run_module_cycle(const PluginModule &module,
                                                        const yaddnsc_host_services &services,
                                                        std::string_view driver_param_json,
                                                        std::string_view ip_addr = "192.0.2.1",
                                                        std::string_view rd_type = "A",
                                                        std::string_view domain = "example.com",
                                                        std::string_view subdomain = "www",
                                                        std::string_view fqdn = "www.example.com") {
    ModuleCycleResult result;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver *handle = nullptr;
    result.create_status = module.create(services, &handle, error);
    if (result.create_status != YADDNSC_STATUS_OK) {
        if (error.message.data != nullptr) {
            result.error_message = std::string(error.message.data, error.message.size);
        }
        return result;
    }

    const auto request = make_update_request(ip_addr, rd_type, domain, subdomain, fqdn, driver_param_json);
    result.update_status = module.update(handle, request, error);
    if (error.message.data != nullptr) {
        result.error_message = std::string(error.message.data, error.message.size);
    }
    result.retry_after_seconds = error.retry_after_seconds;

    module.destroy(handle);
    return result;
}

#endif // YADDNSC_TEST_PLUGIN_PLUGIN_TEST_DOUBLES_H
