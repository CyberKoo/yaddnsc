#ifndef YADDNSC_INFRASTRUCTURE_CORO_CANCELLED_H
#define YADDNSC_INFRASTRUCTURE_CORO_CANCELLED_H

#include "infrastructure/coro/fwd.h"

namespace coro {

/// Runtime cancellation control flow, deliberately not a std::exception.
/// Propagates to the initiating scope after RAII cleanup. Catch only to clean
/// up and rethrow, or at a documented scope / plugin ABI boundary.
class Cancelled final {
private:
    friend class CancelScope;

    explicit Cancelled(const CancelScope* origin) noexcept : origin_(origin) {}

    const CancelScope* origin_;
};

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_CANCELLED_H
