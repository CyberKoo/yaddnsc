//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_WORKER_H
#define YADDNSC_WORKER_H

#include <vector>
#include <optional>
#include <string_view>

#include "config.h"

struct driver_request_t;

class Worker {
public:
    explicit Worker(const Config::domains_config_t &);

    ~Worker() = default;

    Worker(Worker &&) = default;

    void run();

private:
    class Impl;

    struct ImplDeleter {
        void operator()(Impl *);
    };

    std::unique_ptr<Impl, ImplDeleter> _impl;
};

#endif //YADDNSC_WORKER_H
