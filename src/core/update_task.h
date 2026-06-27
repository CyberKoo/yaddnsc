//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATE_TASK_H
#define YADDNSC_CORE_UPDATE_TASK_H

#include <chrono>
#include <string>

#include "config/config.h"

// ---------------------------------------------------------------------------
// UpdateTask — a self-contained value type describing one DNS record update
//              that the Updater should carry out.
// ---------------------------------------------------------------------------
struct UpdateTask {
    Config::subdomain_config subdomain;
    std::string domain_name;
    std::string driver_name;
    std::string fqdn;
    bool force_update{false};
};

// ---------------------------------------------------------------------------
// SubdomainEntry — a single node in the scheduling min-heap.
// ---------------------------------------------------------------------------
struct SubdomainEntry {
    std::chrono::steady_clock::time_point deadline;
    int update_interval;
    int force_update_interval;
    size_t domain_idx; // index into Manager::Impl::domain_states_
    UpdateTask task;

    // std::priority_queue is a max-heap by default, so we invert the comparison.
    bool operator>(const SubdomainEntry &other) const {
        return deadline > other.deadline;
    }
};

// ---------------------------------------------------------------------------
// DomainState — tracks force-update timing for one domain.
// ---------------------------------------------------------------------------
struct DomainState {
    std::chrono::steady_clock::time_point last_force_update;
};

#endif // YADDNSC_CORE_UPDATE_TASK_H
