#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_PACKAGE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_PACKAGE_H

#include <memory>

#include <yaddnsc/sdk/driver_abi.h>

#include "infrastructure/plugin/detail/entry_points.h"
#include "infrastructure/plugin/plugin_package.h"

namespace plugin {

/// Immutable synchronous protocol package. The ABI trampolines are internal to
/// the sync session; application consumers use only PluginPackage.
class SyncPluginPackage final : public PluginPackage {
public:
    SyncPluginPackage(std::shared_ptr<SharedLibrary> library, PluginDescriptor descriptor,
                      domain::DriverCapabilities capabilities, SyncEntryPoints entries);

    [[nodiscard]] bool supports_validate() const noexcept override { return entries_.validate != nullptr; }

    [[nodiscard]] std::string_view validation_entry_name() const noexcept override { return "yaddnsc_driver_validate"; }

    [[nodiscard]] std::unique_ptr<DriverSession> create_update_session(domain::DriverUpdateCommand command,
                                                                       SessionDependencies dependencies) const override;
    [[nodiscard]] std::unique_ptr<DriverSession> create_validation_session(
        std::string driver_param_json, SessionDependencies dependencies) const override;

    [[nodiscard]] yaddnsc_status create(const yaddnsc_host_services& services, yaddnsc_driver** out_driver,
                                        yaddnsc_error& out_error) const;
    void destroy(yaddnsc_driver* driver) const noexcept;
    [[nodiscard]] yaddnsc_status update(yaddnsc_driver* driver, const yaddnsc_update_request& request,
                                        yaddnsc_error& out_error) const;
    [[nodiscard]] yaddnsc_status validate(yaddnsc_driver* driver, yaddnsc_string driver_param_json,
                                          yaddnsc_error& out_error) const;

private:
    SyncEntryPoints entries_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_PACKAGE_H
