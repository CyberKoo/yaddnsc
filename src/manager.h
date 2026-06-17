//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_MANAGER_H
#define YADDNSC_MANAGER_H

#include <memory>
#include <stop_token>

#include "config.h"

struct AppContext;

class Manager {
public:
    explicit Manager(std::shared_ptr<AppContext>, Config::config);

    ~Manager();

    void validate_config() const;

    void load_drivers() const;

    void create_worker() const;

    void run(std::stop_token) const;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_MANAGER_H
