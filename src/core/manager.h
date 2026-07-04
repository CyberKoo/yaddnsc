//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_CORE_MANAGER_H
#define YADDNSC_CORE_MANAGER_H

#include <memory>
#include <stop_token>

#include "config/config.h"
#include "mixin.h"

class Manager {
public:
    explicit Manager(Config::AppConfig config, std::stop_source stop_source);

    ~Manager();

    void load_drivers() const;

    void validate_config() const;

    // Run the scheduler loop.  Blocks until a stop is requested.
    void run() const;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

#endif //YADDNSC_CORE_MANAGER_H
