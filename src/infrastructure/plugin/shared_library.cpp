//
// Created by Kotarou on 2026/9/17.
//

#include "shared_library.h"

#include <utility>

#include <dlfcn.h>

SharedLibrary::SharedLibrary(void *handle, std::string path) noexcept : handle_(handle), path_(std::move(path)) {
}

std::expected<SharedLibrary, std::string> SharedLibrary::open(const std::string &path) noexcept {
    void *handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *error = ::dlerror();
        return std::unexpected(error != nullptr ? error : "unknown dlopen error");
    }
    return SharedLibrary{handle, path};
}

SharedLibrary::~SharedLibrary() {
    if (handle_ != nullptr) {
        ::dlclose(handle_);
    }
}

SharedLibrary::SharedLibrary(SharedLibrary &&other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), path_(std::move(other.path_)) {
}

SharedLibrary &SharedLibrary::operator=(SharedLibrary &&other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            ::dlclose(handle_);
        }
        handle_ = std::exchange(other.handle_, nullptr);
        path_ = std::move(other.path_);
    }
    return *this;
}

void *SharedLibrary::resolve(const char *name) const noexcept {
    ::dlerror(); // clear any stale error before calling dlsym
    void *symbol = ::dlsym(handle_, name);
    return ::dlerror() != nullptr ? nullptr : symbol;
}
