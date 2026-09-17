//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_INSTANCE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_INSTANCE_H

#include <memory>

#include "plugin_loader.h"

/// DriverInstance — one live driver instance plus a lease on its module.
///
/// The shared_ptr lease guarantees the module (and thus the code backing the
/// instance) outlives the instance even if the catalog entry is concurrently
/// removed. The destructor calls the plugin's destroy() while the lease is
/// still held, so no code is ever unloaded under a live instance.
class DriverInstance {
public:
    DriverInstance(std::shared_ptr<const PluginModule> module, yaddnsc_driver *handle) noexcept
        : module_(std::move(module)), handle_(handle) {
    }

    ~DriverInstance() {
        if (handle_ != nullptr) {
            module_->destroy(handle_);
        }
    }

    DriverInstance(DriverInstance &&) = delete;
    DriverInstance &operator=(DriverInstance &&) = delete;
    DriverInstance(const DriverInstance &) = delete;
    DriverInstance &operator=(const DriverInstance &) = delete;

    [[nodiscard]] yaddnsc_status update(const yaddnsc_update_request &request, yaddnsc_error &out_error) const {
        return module_->update(handle_, request, out_error);
    }

    /// Driver-side driver_param validation (optional ABI entry; the module
    /// returns OK when the plugin does not export it).
    [[nodiscard]] yaddnsc_status validate(yaddnsc_string driver_param_json, yaddnsc_error &out_error) const {
        return module_->validate(handle_, driver_param_json, out_error);
    }

private:
    std::shared_ptr<const PluginModule> module_;
    yaddnsc_driver *handle_;
};

#endif // YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_INSTANCE_H
