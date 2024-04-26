//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_WORKER_H
#define YADDNSC_WORKER_H

#include <memory>

namespace Config {
    struct domain_config;
    struct resolver_config;
}

class Worker {
public:
    explicit Worker(const Config::domain_config &, const Config::resolver_config &);

    ~Worker();

    Worker(Worker &&) = default;

    void run() const;

    static void set_concurrency(unsigned int);

private:
    class Impl;

    std::unique_ptr<Impl, void(*)(const Impl *)> impl_;
};

#endif //YADDNSC_WORKER_H
