//
// Created by Kotarou on 2022/4/5.
//
#include "driver_manager.h"

#include <dlfcn.h>

#include <string>
#include <algorithm>
#include <filesystem>
#include <type_traits>

#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "driver_ver.h"
#include "driver/magic.h"
#include "interface/driver.h"
#include "exception/bad_driver.h"

// ===========================================================================
//  DriverManager::Impl  —  private implementation
// ===========================================================================

struct DriverManager::Impl {
    /// RAII wrapper for a loaded driver and its shared-library handle.
    struct DriverModule final {
        /// Custom deleter that calls destroy() inside the driver .so,
        /// ensuring allocation and deallocation stay in the same module.
        struct DriverDeleter {
            void (*destroy_)(Driver *) = nullptr;

            void operator()(Driver *p) const noexcept {
                if (destroy_ && p) {
                    destroy_(p);
                }
            }
        };

        /// RAII wrapper for dlopen handle.
        struct HandleCloser {
            static constexpr void operator()(void *ptr) noexcept {
                if (ptr) { dlclose(ptr); }
            }
        };

        using DriverPtr = std::unique_ptr<Driver, DriverDeleter>;
        using HandlePtr = std::unique_ptr<void, HandleCloser>;

        DriverModule(DriverModule &&) = default;

        DriverModule &operator=(DriverModule &&) = default;

        /// Open the shared library and resolve driver symbols.
        explicit DriverModule(const std::string &path);

        /// Return a const reference to the loaded Driver instance.
        [[nodiscard]] const Driver &get() const;

        ~DriverModule() = default;

        /// Resolve a symbol from a shared library and check for errors.
        /// Signature is a function type, e.g. Driver*() or void(Driver*).
        template<typename Signature> requires std::is_function_v<Signature>
        static Signature *resolve_symbol(const HandlePtr &handle, const char *name) {
            dlerror(); // clear previous errors before calling dlsym
            auto sym = reinterpret_cast<Signature *>(dlsym(handle.get(), name));
            if (const auto error = dlerror()) {
                SPDLOG_CRITICAL("Failed to resolve symbol '{}', error: {}", name, error);
                throw BadDriverException(fmt::format("Failed to resolve symbol '{}' in driver: {}", name, error));
            }
            if (!sym) {
                SPDLOG_CRITICAL("Symbol '{}' resolved to null", name);
                throw BadDriverException(fmt::format("Symbol '{}' resolved to null in driver", name));
            }
            SPDLOG_TRACE("Resolved symbol '{}' at {}", name, static_cast<const void *>(sym));
            return sym;
        }

        /// dlopen handle for the shared library.
        HandlePtr handle_;
        /// Wrapped Driver instance with module-aware deleter.
        DriverPtr driver_;
    };

    ~Impl() = default;

    /// Extract the file name from a path (e.g. "/usr/lib/drv.so" → "drv.so").
    static std::string_view get_driver_name(std::string_view path);

    /// Validate, register, and store a loaded driver module.
    void register_driver(DriverModule driver_res, std::string_view driver_lib_name);

    /// Unload a previously registered driver by name.
    void unload_driver(const std::string &name);

    /// Map of driver name → DriverModule (owns the .so handle and Driver).
    std::map<std::string, DriverModule> driver_map_;
};

// ===========================================================================
//  DriverManager::Impl::DriverModule  —  implementations
// ===========================================================================

DriverManager::Impl::DriverModule::DriverModule(const std::string &path) : handle_{dlopen(path.c_str(), RTLD_NOW)},
                                                                           driver_(nullptr, DriverDeleter{}) {
    if (handle_ == nullptr) {
        SPDLOG_CRITICAL("Unable to load driver {}, error: {}", get_driver_name(path), dlerror());
        throw BadDriverException(fmt::format("Failed to load driver: {}", dlerror()));
    }
    SPDLOG_TRACE("Opened shared library '{}' (handle: {})", get_driver_name(path),
                 static_cast<const void *>(handle_.get()));

    // verify magic before doing anything else
    auto magic_func = resolve_symbol<uint64_t()>(handle_, "yaddnsc_drv_magic");
    if (magic_func() != YADDNSC_DRIVER_MAGIC) {
        SPDLOG_CRITICAL("Driver '{}' failed magic verification: not a yaddnsc driver", get_driver_name(path));
        throw BadDriverException(
            fmt::format("Driver '{}' is not a valid yaddnsc driver (magic mismatch)", get_driver_name(path))
        );
    }

    auto create_func = resolve_symbol<Driver*()>(handle_, "create");
    auto destroy_func = resolve_symbol<void(Driver *)>(handle_, "destroy");

    driver_ = DriverPtr(create_func(), DriverDeleter{destroy_func});
}

const Driver &DriverManager::Impl::DriverModule::get() const {
    return *driver_;
}

// ===========================================================================
//  DriverManager::Impl  —  implementations
// ===========================================================================

std::string_view DriverManager::Impl::get_driver_name(std::string_view path) {
    auto pos = path.rfind('/');
    if (pos == std::string_view::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

void DriverManager::Impl::register_driver(DriverModule driver_res, std::string_view driver_lib_name) {
    auto &driver = driver_res.get();

    // validate driver ABI version
    const auto got = driver.get_abi_version();
    constexpr auto required = DRV_ABI_VERSION;
    if (!got.is_compatible_with(required)) {
        SPDLOG_CRITICAL(
            "Failed to load driver '{}' [ABI {}.{}.{}] - incompatible with host [ABI {}.{}.{}]: "
            "need major=={}, minor>={}, patch>={}",
            driver_lib_name,
            got.major, got.minor, got.patch,
            required.major, required.minor, required.patch,
            required.major, required.minor, required.patch
        );

        throw BadDriverException(
            fmt::format("Driver {} ABI {}.{}.{} incompatible with host {}.{}.{}: "
                        "need major=={}, minor>={}, patch>={}",
                        driver_lib_name,
                        got.major, got.minor, got.patch,
                        required.major, required.minor, required.patch,
                        required.major, required.minor, required.patch
            )
        );
    }

    const auto [name, description, author, version] = driver.get_detail();
    const auto driver_name = std::string(name);

    if (const auto [_, inserted] = driver_map_.emplace(driver_name, std::move(driver_res)); !inserted) {
        SPDLOG_WARN("Driver '{}' ({}) is already loaded, skipped", driver_name, driver_lib_name);
        return;
    }

    SPDLOG_INFO("Loaded driver '{}' ({})", driver_name, driver_lib_name);
    SPDLOG_DEBUG("Driver {} ({}), developed by {}, version: {}", driver_name, description, author, version);
}

void DriverManager::Impl::unload_driver(const std::string &name) {
    const auto it = driver_map_.find(name);
    if (it == driver_map_.end()) {
        SPDLOG_CRITICAL("Driver '{}' not found, cannot unload", name);
        throw BadDriverException(fmt::format("Driver '{}' is not loaded, cannot unload", name));
    }

    driver_map_.erase(it);
    SPDLOG_INFO("Unloaded driver '{}'", name);
}

// ===========================================================================
//  DriverManager public API — thin delegation to Impl
// ===========================================================================

DriverManager::DriverManager() : impl_(std::make_unique<Impl>()) {
}

DriverManager::~DriverManager() = default;

const Driver &DriverManager::get_driver(const std::string &name) const {
    const auto &driver_map = impl_->driver_map_;
    if (const auto it = driver_map.find(name); it != driver_map.end()) {
        SPDLOG_TRACE("Driver '{}' found in registry", name);
        return it->second.get();
    }

    SPDLOG_CRITICAL("Driver {} not found", name);
    throw BadDriverException(fmt::format("Driver '{}' is not loaded", name));
}

std::vector<std::string_view> DriverManager::get_loaded_drivers() const {
    std::vector<std::string_view> loaded_drivers;
    std::ranges::transform(
        impl_->driver_map_, std::back_inserter(loaded_drivers), [](const auto &kv) -> std::string_view {
            return kv.first;
        }
    );

    return loaded_drivers;
}

void DriverManager::load_driver(const std::string &path) const {
    if (!std::filesystem::exists(path)) {
        const auto lib_name = Impl::get_driver_name(path);
        SPDLOG_CRITICAL("Failed to load driver {}, file {} not found", lib_name, path);
        throw BadDriverException(fmt::format("Driver library '{}' not found at {}", lib_name, path));
    }

    impl_->register_driver(Impl::DriverModule(path), Impl::get_driver_name(path));
}

void DriverManager::unload_driver(const std::string &name) {
    impl_->unload_driver(name);
}
