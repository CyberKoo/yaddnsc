//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_WORKER_H
#define YADDNSC_WORKER_H

#include <vector>
#include <optional>
#include <string_view>

#include "config.h"
#include "non_copyable.h"

struct request_t;

class Worker {
public:
    explicit Worker(const Config::domains_config_t &domain_config) : _worker_config(domain_config) {};

    ~Worker() = default;

    void run();

private:
    static std::optional<std::string> dns_lookup(std::string_view host, dns_record_t type);

    void run_scheduled_tasks();

    static std::optional<std::string> get_ip_address(const Config::sub_domain_config_t &);

    static std::optional<std::string> update_dns_record(const request_t &request, ip_version_t version, std::string_view nif);

    static std::string record_type_to_string(dns_record_t);

    static ip_version_t record_type_to_ip_ver(dns_record_t type);

private:
    const Config::domains_config_t &_worker_config;

    int _force_update_counter = 0;
};

#endif //YADDNSC_WORKER_H
