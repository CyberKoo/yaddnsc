#ifndef YADDNSC_SDK_DRIVER_HPP
#define YADDNSC_SDK_DRIVER_HPP

/// Synchronous C++ driver helpers and entry points. Existing includes continue
/// to re-export the common HTTP, logging, JSON and Retry-After helpers.
#include <exception>
#include <functional>
#include <memory>
#include <source_location>
#include <type_traits>
#include <utility>

#include <concepts>

#include <yaddnsc/sdk/driver_common.hpp>
#include <yaddnsc/sdk/log.hpp>
#include <yaddnsc/sdk/run_update.hpp>

namespace yaddnsc::sdk {

/// Copyable borrowed handle; the host table lives from create through destroy.
class Services {
public:
    using LogEnabledFn = detail::LogEnabledFn;

    explicit Services(const yaddnsc_host_services* services) noexcept : services_(services) {}

    Services(const yaddnsc_host_services* services, LogEnabledFn log_enabled) noexcept
        : services_(services), log_enabled_(log_enabled) {}

    [[nodiscard]] const yaddnsc_host_services* get() const noexcept { return services_; }

    [[nodiscard]] LogEnabledFn log_enabled_fn() const noexcept { return log_enabled_; }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return services_ != nullptr && services_->is_cancelled != nullptr &&
               services_->is_cancelled(services_->context) != 0;
    }

    [[nodiscard]] ExchangeResult exchange(const HttpRequest& request) const {
        if (services_ == nullptr || services_->struct_size < YADDNSC_ABI_VERSION_PREFIX_SIZE ||
            !yaddnsc_abi_provides(services_->abi_major, services_->abi_minor, YADDNSC_DRIVER_ABI_MAJOR, 0) ||
            services_->struct_size < YADDNSC_HOST_SERVICES_MIN_SIZE || services_->context == nullptr ||
            services_->http_exchange == nullptr) {
            return std::unexpected(HttpError{YADDNSC_STATUS_INTERNAL_ERROR, "invalid host services table", 0});
        }
        std::vector<yaddnsc_http_header> c_headers;
        c_headers.reserve(request.headers.size());
        for (const auto& [name, value] : request.headers)
            c_headers.push_back({detail::make_view(name), detail::make_view(value)});
        yaddnsc_http_request c_request{};
        c_request.struct_size = static_cast<uint32_t>(sizeof(c_request));
        c_request.url = detail::make_view(request.url);
        c_request.method = static_cast<yaddnsc_http_method>(request.method);
        c_request.headers = c_headers.empty() ? nullptr : c_headers.data();
        c_request.header_count = c_headers.size();
        c_request.content_type = detail::make_view(request.content_type);
        if (request.body)
            c_request.body = detail::make_bytes(*request.body);
        yaddnsc_http_response c_response{};
        c_response.struct_size = static_cast<uint32_t>(sizeof(c_response));
        yaddnsc_error c_error{};
        c_error.struct_size = static_cast<uint32_t>(sizeof(c_error));
        const auto status = services_->http_exchange(services_->context, &c_request, &c_response, &c_error);
        if (!yaddnsc_status_is_valid(status))
            return std::unexpected(HttpError{YADDNSC_STATUS_INTERNAL_ERROR, "host returned an unknown status", 0});
        if (status != YADDNSC_STATUS_OK) {
            if (!detail::has_valid_error(c_error) || c_error.status != status)
                return std::unexpected(
                    HttpError{YADDNSC_STATUS_INTERNAL_ERROR, "host returned an invalid error report", 0});
            return std::unexpected(
                HttpError{status, std::string(detail::to_view(c_error.message)), c_error.retry_after_seconds});
        }
        if (!detail::has_valid_response(c_response))
            return std::unexpected(
                HttpError{YADDNSC_STATUS_INTERNAL_ERROR, "host returned an invalid HTTP response", 0});
        HttpResponse response;
        response.status_code = c_response.status_code;
        response.body = detail::to_view(c_response.body);
        response.headers.reserve(c_response.header_count);
        for (size_t i = 0; i < c_response.header_count; ++i)
            response.headers.push_back(
                {detail::to_view(c_response.headers[i].name), detail::to_view(c_response.headers[i].value)});
        return response;
    }

private:
    const yaddnsc_host_services* services_;
    LogEnabledFn log_enabled_ = nullptr;
};

/// Request views and services borrow the host for one synchronous update.
class UpdateContext {
public:
    using LogEnabledFn = Services::LogEnabledFn;

    UpdateContext(const UpdateRequest& request, const yaddnsc_host_services* services) noexcept
        : UpdateContext(request, services, nullptr) {}

    UpdateContext(const UpdateRequest& request, const yaddnsc_host_services* services,
                  LogEnabledFn log_enabled) noexcept
        : request_(request), services_(services), log_enabled_(log_enabled) {}

    [[nodiscard]] const UpdateRequest& request() const noexcept { return request_; }

    [[nodiscard]] Services services() const noexcept { return Services{services_, log_enabled_}; }

    [[nodiscard]] ExchangeResult exchange(const HttpRequest& request) const { return services().exchange(request); }

    [[nodiscard]] bool is_cancelled() const noexcept { return services().is_cancelled(); }

private:
    UpdateRequest request_;
    const yaddnsc_host_services* services_;
    LogEnabledFn log_enabled_ = nullptr;
};

class Driver : public DriverHelpers {
public:
    virtual ~Driver() noexcept = default;
    /// Distinct instances may update concurrently. No C++ objects cross the ABI.
    virtual Result update(UpdateContext& context) = 0;
    /// Fresh-instance configuration check; must not perform HTTP.
    [[nodiscard]] virtual Result validate(std::string_view driver_param_json) const = 0;

protected:
    template<typename CheckResponse>
    static Result run_update(UpdateContext& context, std::string_view driver_name, const HttpRequest& request,
                             CheckResponse&& check_response,
                             const std::source_location& location = std::source_location::current()) {
        const auto& params = context.request();
        const auto file = location.file_name();
        const auto line = static_cast<int32_t>(location.line());
        const auto function = location.function_name();
        detail::log_lazy([&] { return YADDNSC_LOG_DEBUG; }, [&] { return context.services(); },
                         [&](const auto* services, yaddnsc_log_level level) {
                             detail::log_message(
                                 services, level, file, line, function,
                                 "Domain \"{}\" ({}) received DNS record update request from driver '{}', {}",
                                 params.fqdn, params.record_type, driver_name, format_request(request));
                         });
        auto response = context.exchange(request);
        if (!response) {
            detail::log_lazy([&] { return YADDNSC_LOG_WARN; }, [&] { return context.services(); },
                             [&](const auto* services, yaddnsc_log_level level) {
                                 detail::log_message(services, level, file, line, function,
                                                     "Domain \"{}\" ({}) update failed: {}", params.fqdn,
                                                     params.record_type, response.error().message);
                             });
            return std::unexpected(
                Error{response.error().status, response.error().message, response.error().retry_after_seconds});
        }
        auto checked = std::invoke(std::forward<CheckResponse>(check_response), *response, context.services());
        if (!checked) {
            // Every checker form logs the rejection here, so the domain
            // context stays in the log next to the provider detail the
            // driver reported.
            detail::log_lazy([&] { return YADDNSC_LOG_WARN; }, [&] { return context.services(); },
                             [&](const auto* services, yaddnsc_log_level level) {
                                 detail::log_message(services, level, file, line, function,
                                                     "Domain \"{}\" ({}) update rejected by upstream", params.fqdn,
                                                     params.record_type);
                             });
            if constexpr (std::same_as<decltype(checked), Result>) {
                return checked;
            }
            const auto failure = detail::classify_upstream_failure(*response);
            return std::unexpected(
                Error{failure.status,
                      fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn, params.record_type),
                      failure.retry_after_seconds});
        }
        return {};
    }
};

}  // namespace yaddnsc::sdk

/// Emit ABI descriptor and the sync execution group, including optional validation.
#define YADDNSC_DEFINE_DRIVER(DriverClass, driver_name, driver_description, driver_author, driver_version,             \
                              driver_capabilities, minimum_abi_minor)                                                  \
    namespace {                                                                                                        \
    static_assert((minimum_abi_minor) >= 0 && (minimum_abi_minor) <= YADDNSC_DRIVER_ABI_MINOR);                        \
    constexpr yaddnsc_driver_descriptor YADDNSC_SDK_DESCRIPTOR = {                                                     \
        .struct_size = sizeof(yaddnsc_driver_descriptor),                                                              \
        .abi_major = YADDNSC_DRIVER_ABI_MAJOR,                                                                         \
        .abi_minor = (minimum_abi_minor),                                                                              \
        .magic = YADDNSC_DRIVER_MAGIC,                                                                                 \
        .name = ::yaddnsc::sdk::detail::descriptor_string(driver_name),                                                \
        .version = ::yaddnsc::sdk::detail::descriptor_string(driver_version),                                          \
        .author = ::yaddnsc::sdk::detail::descriptor_string(driver_author),                                            \
        .description = ::yaddnsc::sdk::detail::descriptor_string(driver_description),                                  \
        .capabilities = (driver_capabilities),                                                                         \
    };                                                                                                                 \
    }                                                                                                                  \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(                                        \
        const yaddnsc_driver_descriptor** out_descriptor) {                                                            \
        if (out_descriptor == nullptr)                                                                                 \
            return YADDNSC_STATUS_INVALID_ARGUMENT;                                                                    \
        *out_descriptor = &YADDNSC_SDK_DESCRIPTOR;                                                                     \
        return YADDNSC_STATUS_OK;                                                                                      \
    }                                                                                                                  \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_create(                                                \
        const yaddnsc_host_services* services, yaddnsc_driver** out_driver, yaddnsc_error* out_error) {                \
        return ::yaddnsc::sdk::detail::create_driver<DriverClass, minimum_abi_minor>(services, out_driver, out_error); \
    }                                                                                                                  \
    extern "C" YADDNSC_SDK_EXPORT void yaddnsc_driver_destroy(yaddnsc_driver* driver) {                                \
        ::yaddnsc::sdk::detail::destroy_driver(driver);                                                                \
    }                                                                                                                  \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_update(                                                \
        yaddnsc_driver* driver, const yaddnsc_update_request* request, yaddnsc_error* out_error) {                     \
        return ::yaddnsc::sdk::detail::update_driver(driver, request, out_error);                                      \
    }                                                                                                                  \
    extern "C" YADDNSC_SDK_EXPORT yaddnsc_status yaddnsc_driver_validate(                                              \
        yaddnsc_driver* driver, yaddnsc_string driver_param_json, yaddnsc_error* out_error) {                          \
        return ::yaddnsc::sdk::detail::validate_driver(driver, driver_param_json, out_error);                          \
    }

/// The ABI entry machinery the macro above expands to; host-side, not part of
/// the interface a driver author writes against. Included last so that
/// including <yaddnsc/sdk/driver.hpp> alone still resolves it.
#include <yaddnsc/sdk/detail/driver_instance.hpp>  // IWYU pragma: export

#endif  // YADDNSC_SDK_DRIVER_HPP
