//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_MANAGER_H
#define YADDNSC_MANAGER_H

#include <vector>
#include <thread>

#include "config.h"

class Manager {
public:
    explicit Manager(Config::config_t config);

    ~Manager() = default;

    void validate_config();

    void load_drivers() const;

    void create_worker();

    void run();

private:
    class Impl;

    struct ImplDeleter {
        void operator()(Impl *);
    };

    std::unique_ptr<Impl, ImplDeleter> _impl;
};

#endif //YADDNSC_MANAGER_H
