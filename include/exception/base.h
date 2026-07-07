//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_EXCEPTION_BASE_H
#define YADDNSC_EXCEPTION_BASE_H

#include <stdexcept>
#include <string_view>

/// Base exception class for all yaddnsc-specific errors.
///
/// Inherits from std::runtime_error and provides a virtual get_name()
/// method so that catch sites can identify the concrete exception type
/// without RTTI.
class YaddnscException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;

    /// Return the concrete exception type name (e.g. "BadDriverException").
    [[nodiscard]] virtual std::string_view get_name() const noexcept = 0;
};

#endif //YADDNSC_EXCEPTION_BASE_H
