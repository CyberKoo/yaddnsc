//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DOMAIN_UPDATE_SCHEDULE_QUEUE_H
#define YADDNSC_DOMAIN_UPDATE_SCHEDULE_QUEUE_H

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "domain/config/runtime_config.h"
#include "domain/update/time_types.h"
#include "domain/update/update_task.h"

namespace domain {

/// Identifies one scheduled task by its position in the runtime config.
struct TaskId {
    std::size_t domain_index{};
    std::size_t subdomain_index{};

    bool operator==(const TaskId &) const = default;
};

/// ScheduleQueue — pure timer queue for periodic DDNS update tasks
/// (refactor/phase-3-scheduling-and-workflow.md §3.1).
///
/// Every time value is injected by the caller; the queue never reads a clock,
/// logs, or touches synchronisation primitives. It is NOT thread-safe: the
/// owning scheduler runner drives it from a single sequence.
///
/// Locked behaviours (Phase 0 table):
///   - pop-and-reschedule: pop_due() returns every due task and immediately
///     re-queues it with `now + update_interval` — it does not wait for the
///     task to finish.
///   - first force: when a domain's force_update interval is positive, the
///     first pop of each of its tasks is a forced update (last_force_update
///     starts one full interval in the past, exactly like the legacy
///     Scheduler constructor).
///   - force_update is a domain-level interval; update_interval is the
///     per-subdomain effective value (normaliser already applied overrides).
class ScheduleQueue {
public:
    /// Populate the queue from the runtime config; every entry's first
    /// deadline is `now` (dispatch immediately on startup).
    /// @throws std::invalid_argument if an effective update_interval is
    ///         non-positive (defence in depth; static validation normally
    ///         rejects such configs first).
    ScheduleQueue(std::shared_ptr<const RuntimeConfig> config, TimePoint now);

    /// Return every task whose deadline has passed, re-queueing each one with
    /// its next deadline (`now + update_interval`) before returning. The
    /// force_update flag is evaluated against `now` for each popped task.
    /// Results are ordered by deadline (ties keep config order).
    [[nodiscard]] std::vector<UpdateTask> pop_due(TimePoint now);

    /// Time from `now` until the nearest deadline (clamped at zero), or
    /// std::nullopt when the queue is empty.
    [[nodiscard]] std::optional<Duration> time_until_next(TimePoint now) const;

    /// Move the NEXT deadline of an already-queued task. Never inserts a new
    /// entry. @return false when the task id is unknown.
    bool reschedule(const TaskId &id, TimePoint new_deadline);

    /// Number of scheduled entries (one per subdomain).
    [[nodiscard]] std::size_t size() const;

    /// Whether the queue holds no entries at all.
    [[nodiscard]] bool empty() const;

private:
    struct Entry {
        TimePoint deadline;
        int update_interval{};
        int force_update_interval{};
        TimePoint last_force_update;
        UpdateTask task;
    };

    /// Evaluate the force-update flag for an entry popped at `now`.
    [[nodiscard]] static bool check_force_update(Entry &entry, TimePoint now) noexcept;

    std::shared_ptr<const RuntimeConfig> config_;
    std::vector<Entry> entries_;
};

} // namespace domain

#endif // YADDNSC_DOMAIN_UPDATE_SCHEDULE_QUEUE_H
