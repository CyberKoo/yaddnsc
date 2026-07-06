//
// Created by Kotarou on 2022/4/12.
//

#ifndef YADDNSC_EXCEPTION_CONFIG_VERIFICATION_H
#define YADDNSC_EXCEPTION_CONFIG_VERIFICATION_H

#include "base.h"

/// Thrown when the configuration fails pre-flight validation.
///
/// The message contains a human-readable description of the first violated
/// constraint.
class ConfigVerificationException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const override {
        return "ConfigVerificationException";
    }
};

#endif //YADDNSC_EXCEPTION_CONFIG_VERIFICATION_H
