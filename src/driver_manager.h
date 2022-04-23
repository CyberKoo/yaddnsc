//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_MANAGER_H
#define YADDNSC_DRIVER_MANAGER_H

#include <map>
#include <memory>
#include <vector>
#include <string_view>

#include "non_copyable.h"

class IDriver;

class DriverManager : public NonCopyable {
public:
    ~DriverManager();

    void load_driver(std::string_view);

    std::vector<std::string> get_loaded_drivers();

    std::unique_ptr<IDriver> &get_driver(std::string_view);

private:
    class handle_closer {
    public:
        void operator()(void *);
    };

    using handle_ptr_t = std::unique_ptr<void, handle_closer>;

    [[nodiscard]] static bool is_driver_loaded(std::string_view driver_path);

    static handle_ptr_t load_external_dynamic_library(std::string_view path);

    static IDriver *get_instance(handle_ptr_t &handle);

    static std::string_view get_driver_name(std::string_view path);

private:
    std::map<std::string, std::unique_ptr<IDriver>> _driver_map;

    std::vector<handle_ptr_t> _handlers;
};

#endif //YADDNSC_DRIVER_MANAGER_H
