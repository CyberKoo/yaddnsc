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
    DriverManager();

    ~DriverManager() = default;

    void load_driver(std::string_view);

    std::vector<std::string> get_loaded_drivers();

    std::unique_ptr<IDriver> &get_driver(std::string_view);

private:
    class Impl;

    struct ImplDeleter {
        void operator()(Impl *);
    };

    std::unique_ptr<Impl, ImplDeleter> _impl;
};

#endif //YADDNSC_DRIVER_MANAGER_H
