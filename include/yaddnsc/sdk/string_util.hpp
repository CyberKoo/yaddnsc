//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_SDK_STRING_UTIL_HPP
#define YADDNSC_SDK_STRING_UTIL_HPP

/// String utilities for driver plugins (trim, replace, split/join, case
/// conversion, …). The implementation lives in
/// <yaddnsc/util/string_util.hpp>; this header exposes it under the SDK
/// namespace.

#include <yaddnsc/util/string_util.hpp>

namespace yaddnsc::sdk {

namespace string_util = yaddnsc::util;

} // namespace yaddnsc::sdk

#endif // YADDNSC_SDK_STRING_UTIL_HPP
