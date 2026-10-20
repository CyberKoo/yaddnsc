// Coroutine runtime — the by-type completion registry behind next<T>().
//
// Split out of nursery_state.hpp because it is one concern of the four the
// nursery carries: claim a published result, or decide that no producer of that
// type can publish one any more. The definitions sit below NurseryState because
// they are its members; a translation unit that calls them includes this header,
// and nursery_next_wait.hpp — their only caller — does.
#ifndef YADDNSC_CORO_DETAIL_NURSERY_CLAIMS_HPP
#define YADDNSC_CORO_DETAIL_NURSERY_CLAIMS_HPP

#include "coro/detail/execution_record.h"
#include "coro/detail/nursery_state.hpp"
#include "coro/detail/result_state.hpp"

namespace coro::detail {

inline ChildEntryBase* NurseryState::claim_completed(const void* tag) noexcept {
    ChildEntryBase* entry = completed_head;
    while (entry != nullptr) {
        ChildEntryBase* const next = entry->completed_next;
        if (entry->type_tag == tag && entry->available()) {
            if (entry->result()->content() == ResultContent::CANCELLED) {
                unlink_completed(*entry);
            } else if (entry->result()->reserve()) {
                return entry;
            }
        }
        entry = next;
    }
    return nullptr;
}

inline bool NurseryState::has_pending(const void* tag) const noexcept {
    for (const auto& entry : children) {
        if (entry->owner == nullptr || entry->type_tag != tag) {
            continue;
        }
        if (entry->record.phase() == ExecutionPhase::RETIRED) {
            if (entry->result()->consumption() == ResultConsumption::RESERVED) {
                return true;
            }
            continue;  // consumed, expired, or a cancelled result next<T>() skips
        }
        if (entry->available()) {
            return true;
        }
    }
    return false;
}

inline void NurseryState::release_claim(ChildEntryBase& entry) noexcept {
    if (entry.owner == nullptr) {
        return;
    }
    if (entry.result()->consumption() == ResultConsumption::RESERVED) {
        entry.result()->release_reservation();
        wake_next(entry.type_tag);
    }
}

inline void NurseryState::add_next_waiter(NextWaiter& waiter) noexcept {
    waiter.next = next_head;
    if (next_head != nullptr) {
        next_head->prev = &waiter;
    }
    next_head = &waiter;
    waiter.linked = true;
}

inline void NurseryState::remove_next_waiter(NextWaiter& waiter) noexcept {
    if (!waiter.linked) {
        return;
    }
    if (waiter.prev != nullptr) {
        waiter.prev->next = waiter.next;
    } else {
        next_head = waiter.next;
    }
    if (waiter.next != nullptr) {
        waiter.next->prev = waiter.prev;
    }
    waiter.next = nullptr;
    waiter.prev = nullptr;
    waiter.linked = false;
}

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_NURSERY_CLAIMS_HPP
