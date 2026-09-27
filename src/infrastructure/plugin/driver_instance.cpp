//
// Created by Kotarou on 2026/9/27.
//

#include "driver_instance.h"

DriverInstance::~DriverInstance() noexcept {
    if (handle_ != nullptr) {
        module_->destroy(handle_);
    }
}
