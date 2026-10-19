#ifndef YADDNSC_SDK_DETAIL_ASYNC_INSTANCE_HPP
#define YADDNSC_SDK_DETAIL_ASYNC_INSTANCE_HPP

/// Host-side ABI wiring for the asynchronous execution group.
///
/// `AsyncExchangeHost` and the `ExchangeAwaitable` mutators form the seam
/// between the host event protocol and a suspended driver coroutine; the
/// `detail` block owns the async instance and the create helper. None of this
/// is interface a driver author writes against — a driver awaits
/// `AsyncUpdateContext::exchange` and never names a host type or calls a
/// mutator. `<yaddnsc/sdk/async_driver.hpp>` includes this file last so the
/// single-header path keeps working for existing plugin sources.

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <yaddnsc/sdk/async_driver.hpp>

namespace yaddnsc::sdk {

/// Internal hand-off from an awaitable to its owning instance.
/// Host only — a driver author never implements or calls this.
class AsyncExchangeHost {
public:
    virtual ~AsyncExchangeHost() = default;
    /// True: the coroutine is suspended until an event arrives. False: the
    /// awaitable already holds an error and the coroutine may continue.
    [[nodiscard]] virtual bool start_exchange(ExchangeAwaitable& awaitable, std::coroutine_handle<> handle) = 0;
};

/* ── Host-only awaitable wiring ───────────────────────────────────────────
 * `await_suspend` hands the prepared request to the owning instance, and the
 * mutators let the host deliver one response, error or cancellation into a
 * suspended awaitable. All of it needs the complete AsyncExchangeHost type,
 * which is why these definitions live here rather than beside the await
 * protocol they serve. A driver awaits the object; it never calls these.
 */

inline bool ExchangeAwaitable::await_suspend(std::coroutine_handle<> handle) {
    try {
        prepare_request();
        return host_->start_exchange(*this, handle);
    } catch (...) {
        if (!error_message_) {
            error_message_ = "async exchange preparation failed";
        }
        result_ = std::unexpected(HttpError{YADDNSC_STATUS_INTERNAL_ERROR, *error_message_, 0});
        return false;
    }
}

inline const yaddnsc_http_request& ExchangeAwaitable::c_request() const noexcept {
    return c_request_;
}

inline void ExchangeAwaitable::set_response(const yaddnsc_http_response& response) {
    AsyncHttpResponse value;
    value.status_code = response.status_code;
    value.body = std::string(detail::to_view(response.body));
    value.headers.reserve(response.header_count);
    for (size_t i = 0; i < response.header_count; ++i) {
        value.headers.push_back({std::string(detail::to_view(response.headers[i].name)),
                                 std::string(detail::to_view(response.headers[i].value))});
    }
    result_ = std::move(value);
}

inline void ExchangeAwaitable::set_error(yaddnsc_status status, const yaddnsc_error& error) {
    error_message_ = std::string(detail::to_view(error.message));
    result_ = std::unexpected(HttpError{status, *error_message_, error.retry_after_seconds});
}

inline void ExchangeAwaitable::set_cancelled() {
    error_message_ = "the request was cancelled";
    result_ = std::unexpected(HttpError{YADDNSC_STATUS_CANCELLED, *error_message_, 0});
}

inline void ExchangeAwaitable::set_internal_error(std::string message) {
    error_message_ = std::move(message);
    result_ = std::unexpected(HttpError{YADDNSC_STATUS_INTERNAL_ERROR, *error_message_, 0});
}

namespace detail {

struct OwnedUpdateRequest {
    std::string ip_address;
    std::string record_type;
    std::string domain;
    std::string subdomain;
    std::string fqdn;
    std::string driver_param_json;

    [[nodiscard]] UpdateRequest view() const noexcept {
        return UpdateRequest{
            .ip_address = ip_address,
            .record_type = record_type,
            .domain = domain,
            .subdomain = subdomain,
            .fqdn = fqdn,
            .driver_param_json = driver_param_json,
        };
    }
};

inline bool valid_services(const yaddnsc_async_services* services, uint16_t minimum_minor) noexcept {
    return services != nullptr && services->struct_size >= YADDNSC_ASYNC_SERVICES_MIN_SIZE &&
           services->abi_major == YADDNSC_DRIVER_ABI_MAJOR && services->abi_minor >= minimum_minor &&
           services->context != nullptr && services->log != nullptr && services->is_cancelled != nullptr &&
           services->http_start != nullptr && services->http_cancel != nullptr;
}

template<typename DriverClass>
class AsyncInstance final : public AsyncExchangeHost {
public:
    explicit AsyncInstance(const yaddnsc_async_services* services) : c_services_(services), services_(services, this) {
        if constexpr (std::is_constructible_v<DriverClass, AsyncServices>) {
            driver_ = std::make_unique<DriverClass>(services_);
        } else {
            driver_ = std::make_unique<DriverClass>();
        }
    }

    AsyncInstance(const AsyncInstance&) = delete;
    AsyncInstance& operator=(const AsyncInstance&) = delete;

    [[nodiscard]] yaddnsc_status begin(const yaddnsc_update_request& request, yaddnsc_error* out_error) {
        if (started_ || terminal_ || validated_) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "async instance already used");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        if (!valid_update_request(request)) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async update request");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        try {
            owned_.emplace(OwnedUpdateRequest{
                .ip_address = std::string(to_view(request.ip_address)),
                .record_type = std::string(to_view(request.record_type)),
                .domain = std::string(to_view(request.domain)),
                .subdomain = std::string(to_view(request.subdomain)),
                .fqdn = std::string(to_view(request.fqdn)),
                .driver_param_json = std::string(to_view(request.driver_param_json)),
            });
            context_.emplace(owned_->view(), services_);
            task_ = driver_->update(*context_);
            started_ = true;
            return start_task(out_error);
        } catch (const ConfigParseError& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid configuration"), 0);
            return YADDNSC_STATUS_INVALID_CONFIG;
        } catch (const std::exception& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                        copy_bounded(e.what(), "unknown exception starting async update"));
            return YADDNSC_STATUS_INTERNAL_ERROR;
        } catch (...) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "unknown exception starting async update");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
    }

    [[nodiscard]] yaddnsc_status resume(const yaddnsc_event& event, yaddnsc_error* out_error) {
        if (!started_ || terminal_ || pending_awaitable_ == nullptr) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "async instance is not waiting for an event");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        if (event.struct_size < YADDNSC_EVENT_MIN_SIZE || event.abi_major != YADDNSC_DRIVER_ABI_MAJOR ||
            event.abi_minor < 1) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async event");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }

        if (event.kind == YADDNSC_EVENT_UPDATE_CANCELLED) {
            if (event.op_id != 0 || event.status != YADDNSC_STATUS_CANCELLED || event.response != nullptr ||
                event.error == nullptr || event.error->struct_size < YADDNSC_ERROR_MIN_SIZE ||
                event.error->status != YADDNSC_STATUS_CANCELLED || !yaddnsc_string_is_valid(event.error->message)) {
                write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async cancellation event");
                return YADDNSC_STATUS_INVALID_ARGUMENT;
            }
            try {
                pending_awaitable_->set_cancelled();
            } catch (...) {
                terminal_ = true;
                write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async cancellation copy failed");
                return YADDNSC_STATUS_INTERNAL_ERROR;
            }
        } else if (event.kind == YADDNSC_EVENT_HTTP_FINISHED) {
            if (event.op_id != pending_op_id_ || event.status == YADDNSC_STATUS_PENDING ||
                !yaddnsc_status_is_valid(event.status)) {
                write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async completion event");
                return YADDNSC_STATUS_INVALID_ARGUMENT;
            }
            if (event.status == YADDNSC_STATUS_OK) {
                if (event.response == nullptr || event.error != nullptr ||
                    event.response->struct_size < YADDNSC_HTTP_RESPONSE_MIN_SIZE ||
                    !has_valid_response(*event.response)) {
                    write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async response event");
                    return YADDNSC_STATUS_INVALID_ARGUMENT;
                }
                try {
                    pending_awaitable_->set_response(*event.response);
                } catch (...) {
                    terminal_ = true;
                    write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async response copy failed");
                    return YADDNSC_STATUS_INTERNAL_ERROR;
                }
            } else {
                if (event.error == nullptr || event.response != nullptr ||
                    event.error->struct_size < YADDNSC_ERROR_MIN_SIZE || event.error->status != event.status ||
                    !yaddnsc_string_is_valid(event.error->message)) {
                    write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid async error event");
                    return YADDNSC_STATUS_INVALID_ARGUMENT;
                }
                try {
                    pending_awaitable_->set_error(event.status, *event.error);
                } catch (...) {
                    terminal_ = true;
                    write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async error copy failed");
                    return YADDNSC_STATUS_INTERNAL_ERROR;
                }
            }
        } else {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "unknown async event kind");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }

        const std::coroutine_handle<> handle = std::exchange(pending_handle_, {});
        pending_awaitable_ = nullptr;
        pending_op_id_ = 0;
        try {
            handle.resume();
            return settle(out_error);
        } catch (const ConfigParseError& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid configuration"), 0);
            return YADDNSC_STATUS_INVALID_CONFIG;
        } catch (const std::exception& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded(e.what(), "async task failed"));
            return YADDNSC_STATUS_INTERNAL_ERROR;
        } catch (...) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async task failed");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
    }

    [[nodiscard]] yaddnsc_status validate(yaddnsc_string driver_param_json, yaddnsc_error* out_error) {
        if (!yaddnsc_string_is_valid(driver_param_json)) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "invalid driver_param_json view");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        if (started_ || terminal_ || validated_) {
            write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT, "async instance already used");
            return YADDNSC_STATUS_INVALID_ARGUMENT;
        }
        // validate is the second one-shot operation an instance accepts; mark
        // it before running plugin code so a throw or a failed check still
        // consumes the instance.
        validated_ = true;
        try {
            return translate_result(driver_->validate(to_view(driver_param_json)), error_storage_, out_error);
        } catch (const ConfigParseError& e) {
            write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid configuration"), 0);
            return YADDNSC_STATUS_INVALID_CONFIG;
        } catch (const std::exception& e) {
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                        copy_bounded(e.what(), "unknown exception during async validate"), 0);
            return YADDNSC_STATUS_INTERNAL_ERROR;
        } catch (...) {
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "unknown exception during async validate", 0);
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
    }

    [[nodiscard]] bool start_exchange(ExchangeAwaitable& awaitable, std::coroutine_handle<> handle) override {
        if (pending_awaitable_ != nullptr) {
            awaitable.set_internal_error("another async exchange is already pending");
            return false;
        }
        yaddnsc_error error{};
        error.struct_size = static_cast<uint32_t>(sizeof(error));
        uint64_t op_id = 0;
        const yaddnsc_status status =
            c_services_->http_start(c_services_->context, &awaitable.c_request(), &op_id, &error);
        if (status != YADDNSC_STATUS_OK) {
            awaitable.set_error(status, error);
            return false;
        }
        if (op_id == 0) {
            awaitable.set_internal_error("host returned a zero async op id");
            return false;
        }
        pending_awaitable_ = &awaitable;
        pending_handle_ = handle;
        pending_op_id_ = op_id;
        return true;
    }

private:
    [[nodiscard]] yaddnsc_status start_task(yaddnsc_error* out_error) {
        try {
            task_.resume();
            return settle(out_error);
        } catch (const ConfigParseError& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INVALID_CONFIG, copy_bounded(e.what(), "invalid configuration"), 0);
            return YADDNSC_STATUS_INVALID_CONFIG;
        } catch (const std::exception& e) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, copy_bounded(e.what(), "async task failed"));
            return YADDNSC_STATUS_INTERNAL_ERROR;
        } catch (...) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async task failed");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
    }

    [[nodiscard]] yaddnsc_status settle(yaddnsc_error* out_error) {
        if (pending_awaitable_ != nullptr) {
            return YADDNSC_STATUS_PENDING;
        }
        if (!task_.done()) {
            terminal_ = true;
            write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR, "async task suspended without a pending exchange");
            return YADDNSC_STATUS_INTERNAL_ERROR;
        }
        return finish(out_error);
    }

    [[nodiscard]] yaddnsc_status finish(yaddnsc_error* out_error) {
        terminal_ = true;
        return translate_result(task_.take_result(), error_storage_, out_error);
    }

    static void write_error(yaddnsc_error* out_error, yaddnsc_status status, std::string_view message,
                            uint32_t retry_after_seconds = 0) noexcept {
        yaddnsc::sdk::detail::write_error(out_error, status, message, retry_after_seconds);
    }

    const yaddnsc_async_services* c_services_;
    AsyncServices services_;
    std::unique_ptr<DriverClass> driver_;
    std::optional<OwnedUpdateRequest> owned_;
    std::optional<AsyncUpdateContext> context_;
    Task<Result> task_;
    ExchangeAwaitable* pending_awaitable_ = nullptr;
    std::coroutine_handle<> pending_handle_{};
    uint64_t pending_op_id_ = 0;
    bool started_ = false;
    bool terminal_ = false;
    bool validated_ = false;
    std::string error_storage_;
};

template<typename DriverClass, uint16_t MinimumMinor>
inline yaddnsc_status create_async_driver(const yaddnsc_async_services* services, yaddnsc_async_driver** out_driver,
                                          yaddnsc_error* out_error) noexcept {
    if (services == nullptr || out_driver == nullptr) {
        yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                                          "services and out_driver must not be null", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    *out_driver = nullptr;
    if (!valid_services(services, MinimumMinor)) {
        yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INVALID_ARGUMENT,
                                          "incompatible async host services table", 0);
        return YADDNSC_STATUS_INVALID_ARGUMENT;
    }
    static_assert(std::is_nothrow_destructible_v<DriverClass>,
                  "async driver classes must be nothrow destructible across the C ABI");
    try {
        auto instance = std::make_unique<AsyncInstance<DriverClass>>(services);
        *out_driver = reinterpret_cast<yaddnsc_async_driver*>(instance.release());  // NOLINT
        return YADDNSC_STATUS_OK;
    } catch (const std::exception& e) {
        yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                                          copy_bounded(e.what(), "unknown exception during async construction"), 0);
    } catch (...) {
        yaddnsc::sdk::detail::write_error(out_error, YADDNSC_STATUS_INTERNAL_ERROR,
                                          "unknown exception during async construction", 0);
    }
    return YADDNSC_STATUS_INTERNAL_ERROR;
}

}  // namespace detail
}  // namespace yaddnsc::sdk

#endif  // YADDNSC_SDK_DETAIL_ASYNC_INSTANCE_HPP
