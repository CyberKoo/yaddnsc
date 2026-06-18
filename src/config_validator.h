//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CONFIG_VALIDATOR_H
#define YADDNSC_CONFIG_VALIDATOR_H

#include "config.h"
#include "base_classes.h"

class DriverManager;
class NetworkManager;

// ---------------------------------------------------------------------------
// ConfigValidator — performs all pre-flight checks on the parsed configuration
// before the scheduler starts.
//
// The MinUpdateInterval template parameter controls the minimum allowed update
// interval (in seconds).  The DriverManager and NetworkManager are injected at
// construction time and must outlive the validator.
//
// Each check throws ConfigVerificationException on failure, so a single call
// to validate() either returns normally (all checks pass) or throws at the
// first violated constraint.
// ---------------------------------------------------------------------------
template <int MUpdateInterval>
class ConfigValidator: public RestrictedClass {
public:
    ConfigValidator(const DriverManager &driver_manager, const NetworkManager &network_manager);

    void validate(const Config::config &cfg) const;

private:
    const DriverManager &driver_manager_;
    const NetworkManager &network_manager_;
};

#endif // YADDNSC_CONFIG_VALIDATOR_H
