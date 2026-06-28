//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_CORE_DRIVER_MANAGER_H
#define YADDNSC_CORE_DRIVER_MANAGER_H

#include <memory>
#include <string>
#include <vector>
#include <string_view>

#include "mixin.h"

class Driver;

class DriverManager final {
public:
    DriverManager();

    ~DriverManager();

    void load_driver(const std::string &path) const;

    void unload_driver(const std::string &name);

    [[nodiscard]] std::vector<std::string_view> get_loaded_drivers() const;

    [[nodiscard]] const Driver &get_driver(const std::string &name) const;

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;

    class Impl;

    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_CORE_DRIVER_MANAGER_H
