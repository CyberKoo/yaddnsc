//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_SDK_FORMAT_HPP
#define YADDNSC_SDK_FORMAT_HPP

/// Formatting helpers for driver plugins: std::format with positional
/// arguments plus named-argument replacement ({key} -> value). The
/// implementation lives in <yaddnsc/util/format.hpp>; this header exposes it
/// under the SDK namespace.

#include <yaddnsc/util/format.hpp>

namespace yaddnsc::sdk {

namespace fmt = yaddnsc::util::fmt;

} // namespace yaddnsc::sdk

#endif // YADDNSC_SDK_FORMAT_HPP
