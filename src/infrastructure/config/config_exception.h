#ifndef YADDNSC_INFRASTRUCTURE_CONFIG_CONFIG_EXCEPTION_H
#define YADDNSC_INFRASTRUCTURE_CONFIG_CONFIG_EXCEPTION_H

#include "support/exception.h"

/// Thrown when the configuration cannot be loaded or fails validation.
///
/// The message contains a human-readable description of the failure: the
/// load/parse diagnostic for an unreadable file, or every violated constraint
/// joined into one message for a rejected configuration.
class ConfigException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const noexcept override { return "ConfigException"; }
};

#endif  // YADDNSC_INFRASTRUCTURE_CONFIG_CONFIG_EXCEPTION_H
