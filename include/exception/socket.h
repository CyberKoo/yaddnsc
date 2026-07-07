//
// Created by Kotarou on 2026/7/6.
//

#ifndef YADDNSC_EXCEPTION_SOCKET_H
#define YADDNSC_EXCEPTION_SOCKET_H

#include <cerrno>
#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>

#include "base.h"

/// Thrown when a POSIX socket operation fails.
///
/// Automatically captures errno and the corresponding string description
/// (e.g. "Connection refused", "Socket operation on non-socket").
class SocketException : public YaddnscException {
public:
    /// Construct from errno and an optional contextual message.
    /// @param errnum    The errno value from the failed operation.
    /// @param context   Contextual prefix (e.g. "connect", "send").
    explicit SocketException(int errnum, const std::string &context)
        : YaddnscException(build_message(errnum, context)), errnum_(errnum), has_errno_(true) {
    }

    /// Construct with only a descriptive message (no errno).
    /// Used for conditions like clean EOF where errno is not meaningful.
    explicit SocketException(const std::string &message) : YaddnscException(message), errnum_(0), has_errno_(false) {
    }

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "SocketException";
    }

    /// Return the original errno value (0 if constructed without errno).
    [[nodiscard]] int get_errno() const noexcept {
        return errnum_;
    }

    /// Whether this exception was constructed with an errno.
    [[nodiscard]] bool has_errno() const noexcept {
        return has_errno_;
    }

private:
    int errnum_;
    bool has_errno_;

    static std::string build_message(int errnum, std::string_view context) {
        std::string msg;
        if (!context.empty()) {
            msg += context;
            msg += ": ";
        }
        msg += std::error_code{errnum, std::generic_category()}.message();
        return msg;
    }
};

#endif // YADDNSC_EXCEPTION_SOCKET_H
