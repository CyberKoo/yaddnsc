#include "driver_instance.h"

DriverInstance::~DriverInstance() noexcept {
    if (handle_ != nullptr) {
        package_->destroy(handle_);
    }
}
