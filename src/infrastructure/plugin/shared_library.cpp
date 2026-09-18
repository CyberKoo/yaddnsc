//
// Created by Kotarou on 2026/9/17.
//

#include "shared_library.h"

#include <utility>

#include <dlfcn.h>

SharedLibrary::SharedLibrary(void *handle, std::string path) noexcept : handle_(handle), path_(std::move(path)) {
}

std::expected<SharedLibrary, std::string> SharedLibrary::open(const std::string &path) {
    void *raw_handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (raw_handle == nullptr) {
        const char *error = ::dlerror();
        return std::unexpected(error != nullptr ? error : "unknown dlopen error");
    }

    // Keep the raw handle guarded until the last throwing step (the path
    // copy) has completed; only then hand ownership to the RAII wrapper.
    struct HandleGuard {
        explicit HandleGuard(void *raw) : handle(raw) {}
        ~HandleGuard() {
            if (handle != nullptr) {
                ::dlclose(handle);
            }
        }
        HandleGuard(const HandleGuard &) = delete;
        HandleGuard &operator=(const HandleGuard &) = delete;
        [[nodiscard]] void *release() noexcept { return std::exchange(handle, nullptr); }
        void *handle;
    };
    HandleGuard guard{raw_handle};
    std::string owned_path = path;
    return SharedLibrary{guard.release(), std::move(owned_path)};
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
