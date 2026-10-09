#include "driver_instance.h"

DriverInstance::~DriverInstance() noexcept {
    if (handle_ != nullptr) {
        module_->destroy(handle_);
    }
}
