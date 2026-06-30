//
// Created by Kotarou on 2026/6/30.
//

#ifndef YADDNSC_CERT_UTIL_H
#define YADDNSC_CERT_UTIL_H

#include <string_view>

// Returns the path to the system CA bundle, or empty string if not found.
// The result is cached after the first call.
std::string_view get_system_ca_path();

#endif // YADDNSC_CERT_UTIL_H
