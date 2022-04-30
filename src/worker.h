//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_WORKER_H
#define YADDNSC_WORKER_H

#include <memory>
#include <vector>
#include <optional>
#include <string_view>

namespace Config {
    struct domains_config;
    struct resolver_config;
}

class Worker {
public:
    explicit Worker(const Config::domains_config &, const Config::resolver_config &);

    ~Worker() = default;

    Worker(Worker &&) = default;

    void run();

    static void set_concurrency(unsigned int);

private:
    class Impl;

    struct ImplDeleter {
        void operator()(Impl *);
    };

    std::unique_ptr<Impl, ImplDeleter> impl_;
};

#endif //YADDNSC_WORKER_H
