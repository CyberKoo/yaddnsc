#ifndef YADDNSC_TEST_SUPPORT_CORO_RESERVATION_PRESSURE_HPP
#define YADDNSC_TEST_SUPPORT_CORO_RESERVATION_PRESSURE_HPP

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

#include "coro/coro.h"

namespace test {

struct ReservationPressureResult {
    int errors = 0;
    int cancelled = 0;
    int recovered = 0;
};

namespace reservation_pressure {

template<typename T>
coro::Task<T> produce(int value) {
    co_await coro::sleep_for(std::chrono::milliseconds(1));
    co_return static_cast<T>(value);
}

template<typename T>
coro::Task<void> consume(coro::Nursery& nursery, coro::CancelScope*& slot, std::vector<int>& seen,
                         ReservationPressureResult& result) {
    const auto outcome = co_await coro::with_cancel_scope([&](coro::CancelScope& scope) -> coro::Task<void> {
        slot = &scope;
        auto value = co_await nursery.next<T>();
        if (!value || !*value) {
            ++result.errors;
        } else {
            const auto index = static_cast<std::size_t>(**value);
            if (index >= seen.size()) {
                ++result.errors;
            } else {
                ++seen[index];
            }
        }
    });
    result.cancelled += outcome.cancelled;
    slot = nullptr;
}

// Destroyed by the reaper after all producers publish in the same batch, but
// before any queued consumer can run. This exercises cancellation of claims,
// not just cancellation of waiters that never obtained a result.
struct CancelClaims {
    std::vector<coro::CancelScope*>& scopes;
    bool enabled;
    ReservationPressureResult& result;

    ~CancelClaims() noexcept {
        if (!enabled) {
            return;
        }
        for (std::size_t index = 0; index < scopes.size(); ++index) {
            if (index % 4 < 2) {
                if (scopes[index] == nullptr) {
                    ++result.errors;
                } else {
                    scopes[index]->cancel();
                }
            }
        }
    }
};

inline coro::Task<void> cancel_at_retirement([[maybe_unused]] std::unique_ptr<CancelClaims> cancel) {
    co_await coro::sleep_for(std::chrono::milliseconds(1));
}

template<typename T>
coro::Task<void> recover(coro::Nursery& nursery, std::vector<int>& seen, ReservationPressureResult& result) {
    int previous = -1;
    while (auto value = co_await nursery.next<T>()) {
        if (!*value) {
            ++result.errors;
            continue;
        }
        const int index = static_cast<int>(**value);
        if (index <= previous || index < 0 || static_cast<std::size_t>(index) >= seen.size()) {
            ++result.errors;
        } else {
            ++seen[static_cast<std::size_t>(index)];
        }
        previous = index;
        ++result.recovered;
    }
}

}  // namespace reservation_pressure

// `children` must be a positive multiple of four. Alternate int/long producers
// when mixed, and park one consumer per producer. Every result is reserved in
// the publication batch; cancellation releases half of them in place.
inline coro::Task<ReservationPressureResult> reservation_pressure_batch(int children, bool mixed, bool cancel) {
    ReservationPressureResult result;
    std::vector<int> seen(static_cast<std::size_t>(children));
    std::vector<coro::CancelScope*> scopes(static_cast<std::size_t>(children), nullptr);
    co_await coro::with_nursery([&](coro::Nursery& nursery) -> coro::Task<void> {
        for (int index = 0; index < children; ++index) {
            if (mixed && index % 2 != 0) {
                nursery.spawn(reservation_pressure::produce<long>(index));
            } else {
                nursery.spawn(reservation_pressure::produce<int>(index));
            }
        }
        for (int index = 0; index < children; ++index) {
            auto& slot = scopes[static_cast<std::size_t>(index)];
            if (mixed && index % 2 != 0) {
                nursery.spawn_discard(reservation_pressure::consume<long>(nursery, slot, seen, result));
            } else {
                nursery.spawn_discard(reservation_pressure::consume<int>(nursery, slot, seen, result));
            }
        }
        nursery.spawn_discard(reservation_pressure::cancel_at_retirement(
            std::make_unique<reservation_pressure::CancelClaims>(scopes, cancel, result)));
        // All consumers have resumed and retired before recovery; the owning
        // body is not one of the independently cancelled consumer scopes.
        co_await coro::sleep_for(std::chrono::milliseconds(2));
        co_await reservation_pressure::recover<int>(nursery, seen, result);
        if (mixed) {
            co_await reservation_pressure::recover<long>(nursery, seen, result);
        }
    });
    for (const int count : seen) {
        result.errors += count != 1;
    }
    result.errors += result.cancelled != (cancel ? children / 2 : 0);
    result.errors += result.recovered != (cancel ? children / 2 : 0);
    co_return result;
}

}  // namespace test
#endif  // YADDNSC_TEST_SUPPORT_CORO_RESERVATION_PRESSURE_HPP
