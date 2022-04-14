//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_MANAGER_H
#define YADDNSC_MANAGER_H

#include <vector>
#include <thread>

#include "config.h"
#include "worker.h"

class Manager {
public:
    explicit Manager(Config::config_t config) : _config(std::move(config)) {};

    ~Manager() = default;

    void validate_config();

    void load_drivers() const;

    void create_worker();

    void run();

private:
    Config::config_t _config;

    std::vector<Worker> _workers;

    static constexpr int MIN_UPDATE_INTERVAL = 60;
};


#endif //YADDNSC_MANAGER_H
