//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_MANAGER_H
#define YADDNSC_MANAGER_H

#include "config.h"

class Manager {
public:
    explicit Manager(Config::config);

    ~Manager();

    void validate_config() const;

    void load_drivers() const;

    void create_worker() const;

    void run() const;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_MANAGER_H
