//
// Created by Kotarou on 2026/9/17.
//

#include "schedule_queue.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "domain/fqdn.h"

#include "fmt.hpp"

namespace domain {

ScheduleQueue::ScheduleQueue(std::shared_ptr<const RuntimeConfig> config, TimePoint now)
    : config_(std::move(config)) {
    for (std::size_t domain_idx = 0; domain_idx < config_->domains.size(); ++domain_idx) {
        const auto &domain_config = config_->domains[domain_idx];
        for (std::size_t subdomain_idx = 0; subdomain_idx < domain_config.subdomains.size(); ++subdomain_idx) {
            const auto &subdomain = domain_config.subdomains[subdomain_idx];
            // SubdomainConfig::update_interval is already the effective value
            // (normaliser applied the domain-level fallback).
            const auto effective_interval = subdomain.update_interval;

            // A non-positive interval would re-queue the entry with deadline ==
            // now forever, spinning pop_due() into a busy loop. Static
            // validation normally rejects this, but the queue must not rely
            // on the caller invoking validate_config() (defence in depth).
            if (effective_interval <= 0) {
                throw std::invalid_argument(
                    fmt::format("Update interval for {}.{} must be positive (got {})", subdomain.name,
                                domain_config.name, effective_interval));
            }

            // Initialise last_force_update far enough in the past so that the
            // first pop_due() call always triggers force_update when the
            // interval is positive. Using {} (epoch) would make the elapsed
            // time depend on system uptime, which on a fresh CI runner can be
            // shorter than the force_update_interval.
            const auto force_update_past = domain_config.force_update > 0
                ? now - std::chrono::seconds(domain_config.force_update)
                : TimePoint{};

            entries_.push_back(Entry{
                .deadline = now,
                .update_interval = effective_interval,
                .force_update_interval = domain_config.force_update,
                .last_force_update = force_update_past,
                .task = {
                    .config = config_,
                    .domain_index = domain_idx,
                    .subdomain_index = subdomain_idx,
                    .fqdn = make_fqdn(domain_config.name, subdomain.name),
                    .force_update = false,
                },
            });
        }
    }
}

bool ScheduleQueue::check_force_update(Entry &entry, TimePoint now) noexcept {
    if (entry.force_update_interval <= 0) {
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - entry.last_force_update).count();

    if (elapsed >= entry.force_update_interval) {
        entry.last_force_update = now;
        return true;
    }

    return false;
}

std::vector<UpdateTask> ScheduleQueue::pop_due(TimePoint now) {
    // Worst case every entry is due; reserving avoids reallocating (and
    // re-copying the JSON-bearing SubdomainConfig of already-popped tasks)
    // during catch-up bursts.
    std::vector<std::size_t> due_indices;
    due_indices.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (now >= entries_[i].deadline) {
            due_indices.push_back(i);
        }
    }

    // Match the legacy min-heap semantics: due tasks come out earliest
    // deadline first; ties keep config order (stable sort).
    std::stable_sort(due_indices.begin(), due_indices.end(),
                     [this](std::size_t a, std::size_t b) { return entries_[a].deadline < entries_[b].deadline; });

    std::vector<UpdateTask> due;
    due.reserve(due_indices.size());
    for (const auto idx: due_indices) {
        auto &entry = entries_[idx];

        entry.task.force_update = check_force_update(entry, now);
        // The entry stays queued with its task intact, so the caller receives
        // a copy: the same task value must live in both places.
        due.push_back(entry.task);

        // Re-queue the entry with its next deadline for the next cycle.
        entry.deadline = now + std::chrono::seconds(entry.update_interval);
        entry.task.force_update = false;
    }

    return due;
}

std::optional<Duration> ScheduleQueue::time_until_next(TimePoint now) const {
    if (entries_.empty()) {
        return std::nullopt;
    }

    auto nearest = entries_.front().deadline;
    for (const auto &entry: entries_) {
        nearest = std::min(nearest, entry.deadline);
    }

    return std::max(nearest - now, Duration::zero());
}

bool ScheduleQueue::reschedule(const TaskId &id, TimePoint new_deadline) {
    for (auto &entry: entries_) {
        if (entry.task.domain_index == id.domain_index && entry.task.subdomain_index == id.subdomain_index) {
            entry.deadline = new_deadline;
            return true;
        }
    }
    return false;
}

std::size_t ScheduleQueue::size() const {
    return entries_.size();
}

bool ScheduleQueue::empty() const {
    return entries_.empty();
}

} // namespace domain
