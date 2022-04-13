//
// Created by Kotarou on 2022/4/12.
//

#ifndef YADDNSC_CONFIG_VERIFICATION_EXCEPTION_H
#define YADDNSC_CONFIG_VERIFICATION_EXCEPTION_H

#include "base_exception.h"

#include <string>

class ConfigVerificationException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    [[nodiscard]] std::string_view get_name() const override {
        return "ConfigVerificationException";
    }
};

#endif //YADDNSC_CONFIG_VERIFICATION_EXCEPTION_H
