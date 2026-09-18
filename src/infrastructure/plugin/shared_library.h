//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_SHARED_LIBRARY_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_SHARED_LIBRARY_H

#include <expected>
#include <string>

/// SharedLibrary — move-only RAII wrapper over dlopen/dlclose.
///
/// Opened with RTLD_NOW | RTLD_LOCAL: missing symbols fail at load time
/// rather than on first use, and plugin symbols are never re-exported to
/// other modules (plugins must not bind to each other's Glaze/OpenSSL).
class SharedLibrary {
public:
    SharedLibrary() noexcept = default;

    /// Open the shared library at @p path.
    /// @return The library handle, or the dlerror() text on failure.
    [[nodiscard]] static std::expected<SharedLibrary, std::string> open(const std::string &path);

    ~SharedLibrary();

    SharedLibrary(SharedLibrary &&other) noexcept;
    SharedLibrary &operator=(SharedLibrary &&other) noexcept;

    SharedLibrary(const SharedLibrary &) = delete;
    SharedLibrary &operator=(const SharedLibrary &) = delete;

    /// Resolve a symbol. Returns nullptr when the symbol is absent.
    [[nodiscard]] void *resolve(const char *name) const noexcept;

    [[nodiscard]] const std::string &path() const noexcept { return path_; }

private:
    SharedLibrary(void *handle, std::string path) noexcept;

    void *handle_ = nullptr;
    std::string path_;
};

#endif // YADDNSC_INFRASTRUCTURE_PLUGIN_SHARED_LIBRARY_H
