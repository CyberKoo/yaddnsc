#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_PACKAGE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_PACKAGE_H

#include <memory>

#include <yaddnsc/sdk/driver_abi.h>

#include "infrastructure/plugin/detail/entry_points.h"
#include "infrastructure/plugin/plugin_package.h"

namespace plugin {

/// Immutable asynchronous protocol package. Its ABI trampolines are used only
/// by the async session and never escape into application-facing contracts.
class AsyncPluginPackage final : public PluginPackage {
public:
    AsyncPluginPackage(std::shared_ptr<SharedLibrary> library, PluginDescriptor descriptor,
                       domain::DriverCapabilities capabilities, AsyncEntryPoints entries);

    [[nodiscard]] bool supports_validate() const noexcept override { return entries_.validate != nullptr; }

    [[nodiscard]] std::string_view validation_entry_name() const noexcept override {
        return "yaddnsc_driver_async_validate";
    }

    [[nodiscard]] std::unique_ptr<DriverSession> create_update_session(domain::DriverUpdateCommand command,
                                                                       SessionDependencies dependencies) const override;
    [[nodiscard]] std::unique_ptr<DriverSession> create_validation_session(
        std::string driver_param_json, SessionDependencies dependencies) const override;

    [[nodiscard]] yaddnsc_status async_create(const yaddnsc_async_services& services, yaddnsc_async_driver** out_driver,
                                              yaddnsc_error& out_error) const;
    void async_destroy(yaddnsc_async_driver* driver) const noexcept;
    [[nodiscard]] yaddnsc_status async_begin(yaddnsc_async_driver* driver, const yaddnsc_update_request& request,
                                             yaddnsc_error& out_error) const;
    [[nodiscard]] yaddnsc_status async_resume(yaddnsc_async_driver* driver, const yaddnsc_event& event,
                                              yaddnsc_error& out_error) const;
    [[nodiscard]] yaddnsc_status async_validate(yaddnsc_async_driver* driver, yaddnsc_string driver_param_json,
                                                yaddnsc_error& out_error) const;

private:
    AsyncEntryPoints entries_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_PACKAGE_H
