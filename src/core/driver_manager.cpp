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

#include "driver_interface.h"
#include "driver_ver.h"
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
    class handle_closer {
    public:
        constexpr void operator()(void *ptr) const {
            if (ptr != nullptr) { dlclose(ptr); }
        }
    };

    using handle_ptr = std::unique_ptr<void, handle_closer>;

    Driver(Driver &&) = default;

    Driver &operator=(Driver &&) = default;

    explicit Driver(const std::string &path) : handle_{dlopen(path.c_str(), RTLD_LAZY)} {
        if (handle_ == nullptr) {
            SPDLOG_CRITICAL("Unable to load driver {}, error: {}", get_driver_name(path), dlerror());
            throw BadDriverException(fmt::format("Failed to load driver: {}", dlerror()));
        }

        // reset error message pointer
        dlerror();

        // load create function
        auto create_func = reinterpret_cast<std::add_pointer_t<IDriver *()>>(dlsym(handle_.get(), "create"));
        if (const auto error = dlerror()) {
            SPDLOG_CRITICAL("Failed to create driver instance, error: {}", error);
            throw BadDriverException(fmt::format("Failed to resolve symbol 'create' in driver: {}", error));
        }

        driver_ = std::unique_ptr<IDriver>(create_func());
    }

    std::unique_ptr<IDriver> &get() {
        return driver_;
    }

    ~Driver() override {
        driver_.reset();
        handle_.reset();
    }

private:
    handle_ptr handle_;

    std::unique_ptr<IDriver> driver_;
};

DriverManager::~DriverManager() = default;

IDriver &DriverManager::get_driver(const std::string &name) const {
    if (const auto it = impl_->driver_map_.find(name); it != impl_->driver_map_.end()) {
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
        SPDLOG_CRITICAL("Driver {} has version {} but expected version {}", driver_lib_name, driver->get_driver_version(),
                        DRV_VERSION);

        throw BadDriverException(fmt::format("Driver {} ABI version mismatch: got {}, expected {}",
                                                driver_lib_name, driver->get_driver_version(), DRV_VERSION));
    }

    auto [name, description, author, version] = driver->get_detail();
    SPDLOG_INFO("Driver {} loaded, driver name: {}", driver_lib_name, name);
    // SPDLOG_INFO("Driver {} loaded, description: {}", driver_lib_name, description);
    SPDLOG_DEBUG("Driver {} ({}), developed by {}, version: {}", name, description, author, version);

    driver_map_.emplace(name, std::move(driver_res));
    loaded_lib_.emplace_back(driver_lib_name);
}

DriverManager::DriverManager() : impl_(std::make_unique<Impl>()) {
}
