//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_MANAGER_H
#define YADDNSC_DRIVER_MANAGER_H

#include <memory>
#include <vector>
#include <string_view>

#include "base_classes.h"

class IDriver;

class DriverManager final : public RestrictedClass {
public:
    DriverManager();

    ~DriverManager() override;

    void load_driver(const std::string &) const;

    [[nodiscard]] std::vector<std::string_view> get_loaded_drivers() const;

    [[nodiscard]] IDriver &get_driver(const std::string &) const;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_DRIVER_MANAGER_H
