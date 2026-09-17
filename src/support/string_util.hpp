//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_STRING_UTIL_H
#define YADDNSC_STRING_UTIL_H

/// Host-side alias for the shared string utilities. The implementation lives
/// in <yaddnsc/util/string_util.hpp> (single source shared with driver
/// plugins); this header only preserves the historical `StringUtil::` name
/// used across the host codebase.

#include <yaddnsc/util/string_util.hpp>

namespace StringUtil = yaddnsc::util;

#endif // YADDNSC_STRING_UTIL_H
