//
// Created by Kotarou on 2022/4/5.
//
#include "driver_manager.h"

#include <string>
#include <algorithm>
#include <filesystem>

#include "fmt.hpp"
#include <spdlog/spdlog.h>

// POSIX function
#include <dlfcn.h>

#include "driver_ver.h"
#include "driver_interface.h"
#include "exception/bad_driver_exception.h"

// Internal implementation
class DriverManager::Impl {
public:
    class Driver;

    ~Impl() = default;

    [[nodiscard]] bool is_driver_loaded(std::string_view) const;

    static std::string_view get_driver_name(std::string_view);

    void register_driver(Driver, std::string_view);

public:
    std::map<std::string, Driver> driver_map_;

    std::vector<std::string> loaded_lib_;
};

// Driver RAII class
class DriverManager::Impl::Driver final : public NonCopyable {
public:
    // Custom deleter that calls destroy() inside the driver .so,
    // ensuring allocation and deallocation stay in the same module.
    struct DriverDeleter {
        void (*destroy_)(IDriver*) = nullptr;

        void operator()(IDriver* p) const noexcept {
            if (destroy_ && p) {
                destroy_(p);
            }
        }
    };

    using driver_ptr = std::unique_ptr<IDriver, DriverDeleter>;

    class handle_closer {
    public:
        constexpr void operator()(void *ptr) const {
            if (ptr != nullptr) { dlclose(ptr); }
        }
    };

    using handle_ptr = std::unique_ptr<void, handle_closer>;

    Driver(Driver &&) = default;

    Driver &operator=(Driver &&) = default;

    explicit Driver(const std::string &path) : handle_{dlopen(path.c_str(), RTLD_NOW)}, driver_(nullptr, DriverDeleter{}) {
        if (handle_ == nullptr) {
            SPDLOG_CRITICAL("Unable to load driver {}, error: {}", get_driver_name(path), dlerror());
            throw BadDriverException(fmt::format("Failed to load driver: {}", dlerror()));
        }
        SPDLOG_TRACE("Opened shared library '{}' (handle: {})", get_driver_name(path),
                     static_cast<const void *>(handle_.get()));

        auto create_func = resolve_symbol<IDriver*()>(handle_.get(), "create");
        auto destroy_func = resolve_symbol<void(IDriver*)>(handle_.get(), "destroy");

        driver_ = driver_ptr(create_func(), DriverDeleter{destroy_func});
    }

private:
    // Resolve a symbol from a shared library and check for errors.
    // Signature is a function type, e.g. IDriver*() or void(IDriver*).
    template<typename Signature>
    static Signature* resolve_symbol(void* handle, const char* name) {
        dlerror();  // clear previous errors before calling dlsym
        auto sym = reinterpret_cast<Signature*>(dlsym(handle, name));
        if (const auto error = dlerror()) {
            SPDLOG_CRITICAL("Failed to resolve symbol '{}', error: {}", name, error);
            throw BadDriverException(fmt::format("Failed to resolve symbol '{}' in driver: {}", name, error));
        }
        SPDLOG_TRACE("Resolved symbol '{}' at {}", name, static_cast<const void *>(sym));
        return sym;
    }

public:

    driver_ptr &get() {
        return driver_;
    }

    ~Driver() override = default;

private:
    handle_ptr handle_;

    driver_ptr driver_;
};

DriverManager::~DriverManager() = default;

IDriver &DriverManager::get_driver(const std::string &name) const {
    if (const auto it = impl_->driver_map_.find(name); it != impl_->driver_map_.end()) {
        SPDLOG_TRACE("Driver '{}' found in registry", name);
        return *it->second.get();
    }

    SPDLOG_CRITICAL("Driver {} not found", name);
    throw BadDriverException(fmt::format("Driver '{}' is not loaded", name));
}

std::vector<std::string_view> DriverManager::get_loaded_drivers() const {
    std::vector<std::string_view> loaded_drivers;
    std::ranges::transform(
        impl_->driver_map_, std::back_inserter(loaded_drivers),
        [](const auto &kv) -> std::string_view { return kv.first; }
    );

    return loaded_drivers;
}

void DriverManager::load_driver(const std::string &path) const {
    auto driver_lib_name = Impl::get_driver_name(path);
    SPDLOG_DEBUG("Trying to load driver {}", driver_lib_name);

    if (std::filesystem::exists(path)) {
        if (!impl_->is_driver_loaded(path)) {
            impl_->register_driver(Impl::Driver(path), driver_lib_name);
        } else {
            SPDLOG_WARN("Driver {} is already loaded", driver_lib_name);
        }
    } else {
        SPDLOG_CRITICAL("Failed to load driver {}, file {} not found", driver_lib_name, path);
        throw BadDriverException(fmt::format("Driver library '{}' not found at {}", driver_lib_name, path));
    }
}

std::string_view DriverManager::Impl::get_driver_name(std::string_view path) {
    auto pos = path.rfind('/');
    if (pos == std::string_view::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

bool DriverManager::Impl::is_driver_loaded(std::string_view driver_path) const {
    const auto driver_name = get_driver_name(driver_path);
    return std::ranges::find(loaded_lib_, driver_name) != loaded_lib_.end();
}

void DriverManager::Impl::register_driver(Driver driver_res, std::string_view driver_lib_name) {
    const auto &driver = driver_res.get();

    // validate driver ABI version
    if (driver->get_driver_version() != DRV_VERSION) {
        SPDLOG_CRITICAL("Failed to load driver '{}' [ABI: {}] - version mismatch: got {}, expected {}",
                        driver_lib_name, DRV_VERSION,
                        driver->get_driver_version(), DRV_VERSION);

        throw BadDriverException(fmt::format("Driver {} ABI version mismatch: got {}, expected {}",
                                                driver_lib_name, driver->get_driver_version(), DRV_VERSION));
    }

    auto [name, description, author, version] = driver->get_detail();
    SPDLOG_INFO("Loaded driver '{}' ({}) [ABI: {}]", name, driver_lib_name, DRV_VERSION);
    SPDLOG_DEBUG("Driver {} ({}), developed by {}, version: {}", name, description, author, version);

    driver_map_.emplace(name, std::move(driver_res));
    loaded_lib_.emplace_back(driver_lib_name);
}

DriverManager::DriverManager() : impl_(std::make_unique<Impl>()) {
}
