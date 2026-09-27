//
// Created by Kotarou on 2026/9/27.
//
#include "infrastructure/network/socket_exception.h"

#include <string>
#include <string_view>
#include <system_error>

std::string SocketException::build_message(int errnum, std::string_view context) {
    std::string msg;
    if (!context.empty()) {
        msg += context;
        msg += ": ";
    }
    msg += std::error_code{errnum, std::generic_category()}.message();
    return msg;
}
