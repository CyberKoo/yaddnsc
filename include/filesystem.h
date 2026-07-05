//
// Created by Kotarou on 2026/7/5.
//

#ifndef YADDNSC_FILESYSTEM_H
#define YADDNSC_FILESYSTEM_H

// GCC 7-8: <experimental/filesystem> + std::experimental::filesystem + -lstdc++fs
// GCC 9+: <filesystem> + std::filesystem (no extra link flags)
// Clang with libstdc++: varies, detected via __has_include

#if __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#error "No filesystem header found"
#endif

#endif //YADDNSC_FILESYSTEM_H
