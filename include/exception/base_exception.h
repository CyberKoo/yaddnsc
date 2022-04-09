//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_BASE_EXCEPTION_H
#define YADDNSC_BASE_EXCEPTION_H

#include <stdexcept>

class YaddnscException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;

    [[nodiscard]] virtual std::string_view get_name() const = 0;
};

#endif //YADDNSC_BASE_EXCEPTION_H
