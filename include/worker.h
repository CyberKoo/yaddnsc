//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_WORKER_H
#define YADDNSC_WORKER_H

#include <vector>
#include <optional>
#include <string_view>

#include "config.h"

struct request_t;

class Worker {
public:
    explicit Worker(const Config::domains_config_t &domain_config) : _worker_config(domain_config) {};

    ~Worker() = default;

    void run();

private:
    void run_scheduled_tasks();

    [[nodiscard]] bool is_forced_update() const;

    static std::optional<std::string> dns_lookup(std::string_view host, dns_record_t type);

    static std::optional<std::string> get_ip_address(const Config::sub_domain_config_t &);

    static std::optional<std::string> update_dns_record(const request_t &, ip_version_t, std::string_view);

    static ip_version_t rdtype2ip(dns_record_t);

private:
    const Config::domains_config_t &_worker_config;

    int _force_update_counter = 0;
};

#endif //YADDNSC_WORKER_H
