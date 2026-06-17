//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_WORKER_H
#define YADDNSC_WORKER_H

#include <memory>
#include <stop_token>

namespace Config {
    struct domain_config;
    struct resolver_config;
}

struct AppContext;

class Worker {
public:
    explicit Worker(std::shared_ptr<AppContext>, const Config::domain_config &, const Config::resolver_config &);

    ~Worker();

    Worker(Worker &&) noexcept;

    void run(std::stop_token) const;

    static void set_concurrency(unsigned int);

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_WORKER_H
