#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_DRIVER_INSTANCE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_DRIVER_INSTANCE_H

#include <memory>
#include <utility>

#include <yaddnsc/sdk/driver_abi.h>

#include "infrastructure/plugin/sync/package.h"

/// One live synchronous driver instance plus a lease on its package. The
/// package outlives the matching destroy call even if the catalog is changed.
class DriverInstance {
public:
    DriverInstance(std::shared_ptr<const plugin::SyncPluginPackage> package, yaddnsc_driver* handle) noexcept
        : package_(std::move(package)), handle_(handle) {}

    ~DriverInstance() noexcept;

    DriverInstance(DriverInstance&&) = delete;
    DriverInstance& operator=(DriverInstance&&) = delete;
    DriverInstance(const DriverInstance&) = delete;
    DriverInstance& operator=(const DriverInstance&) = delete;

    [[nodiscard]] yaddnsc_status update(const yaddnsc_update_request& request, yaddnsc_error& out_error) const {
        return package_->update(handle_, request, out_error);
    }

    [[nodiscard]] yaddnsc_status validate(yaddnsc_string driver_param_json, yaddnsc_error& out_error) const {
        return package_->validate(handle_, driver_param_json, out_error);
    }

private:
    std::shared_ptr<const plugin::SyncPluginPackage> package_;
    yaddnsc_driver* handle_;
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_DRIVER_INSTANCE_H
