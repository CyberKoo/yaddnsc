//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_EXCEPTION_BASE_H
#define YADDNSC_EXCEPTION_BASE_H

#include <stdexcept>
#include <string_view>

class YaddnscException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;

    [[nodiscard]] virtual std::string_view get_name() const = 0;
};

#endif //YADDNSC_EXCEPTION_BASE_H
