#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_NOT_FOUND_EXCEPTION_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_NOT_FOUND_EXCEPTION_H

#include "support/exception.h"

/// Thrown when a lookup is performed for a driver name that has not been
/// loaded into the DriverCatalog.
class DriverNotFoundException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const noexcept override { return "DriverNotFoundException"; }
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DRIVER_NOT_FOUND_EXCEPTION_H
