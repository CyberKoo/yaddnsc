//
// Coroutine runtime — internal result storage.
//
// Result storage that outlives the frame that produced it: the producing frame
// is destroyed as soon as its execution retires, while the value, defect or
// cancellation stays consumable. One slot carries three independent states —
// production, content and consumption — because a result is written once,
// published once and taken at most once, and none of the three steps may be
// folded into another.
//
// Nothing here knows about frames, loops or how an outcome is produced. The
// slot is built at its final address before the producer ever touches it
// (invariant I2) and is therefore neither copyable nor movable.
//
// Thread safety: none. A slot is written and read on the loop thread.
//

#ifndef YADDNSC_CORO_DETAIL_RESULT_STATE_HPP
#define YADDNSC_CORO_DETAIL_RESULT_STATE_HPP

#include <cassert>
#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

#include "coro/cancelled.h"

namespace coro::detail {

/// How far the producer has got. READY is published by the loop's reap
/// transaction, never by the producer: until the producing frame is gone, no
/// consumer may observe the result.
enum class ResultProduction : std::uint8_t { PRODUCING, WRITTEN, READY };

/// What a written slot holds. A captured coro::Cancelled is CANCELLED, not a
/// defect — cancellation is control flow that the responsible scope absorbs.
enum class ResultContent : std::uint8_t { VALUE, ERROR, CANCELLED };

/// Whether a consumer already owns the result. A reservation belongs to one
/// wait and is handed back when that wait is cancelled; a consumption ends at
/// CONSUMED (even when the payload's move throws) or EXPIRED.
enum class ResultConsumption : std::uint8_t { AVAILABLE, RESERVED, CONSUMED, EXPIRED };

/// Non-template half of a result slot: the three states, the captured exception
/// and the transitions the runtime drives on them. It exists so a record can
/// carry a type-erased pointer to its result; the payload lives in the derived
/// ResultState<T>.
///
/// The write paths are noexcept: only the derived value write may throw, and it
/// throws T's constructor, not anything of this class.
class ResultStateBase {
public:
    ResultStateBase() = default;

    ResultStateBase(const ResultStateBase&) = delete;
    ResultStateBase& operator=(const ResultStateBase&) = delete;
    ResultStateBase(ResultStateBase&&) = delete;
    ResultStateBase& operator=(ResultStateBase&&) = delete;

    [[nodiscard]] ResultContent content() const noexcept { return content_; }

    [[nodiscard]] ResultConsumption consumption() const noexcept { return consumption_; }

    /// True once the reap transaction published the result. Only then may a
    /// consumer extract the payload.
    [[nodiscard]] bool ready() const noexcept { return production_ == ResultProduction::READY; }

    /// The captured defect or cancellation, meaningful for content ERROR and
    /// CANCELLED. A consumer rethrows it at its own boundary, so the slot only
    /// stores it.
    [[nodiscard]] const std::exception_ptr& error() const noexcept { return error_; }

    /// Store the exception a frame captured and classify it: coro::Cancelled
    /// becomes CANCELLED, every other exception a defect. This is the
    /// unhandled-exception write path, so it may neither throw nor allocate.
    void capture_exception(const std::exception_ptr& error) noexcept {
        error_ = error;
        content_ = ResultContent::ERROR;
        try {
            std::rethrow_exception(error);
        } catch (const Cancelled&) {
            content_ = ResultContent::CANCELLED;
        } catch (...) {
            // Any other exception keeps its defect classification.
        }
        mark_written();
    }

    /// Publish a written result to consumers. Only the loop's reap transaction
    /// may call this, which is what makes READY mean "the producer is gone".
    void publish_ready() noexcept {
        assert(production_ == ResultProduction::WRITTEN && "publishing an unfinished result");
        production_ = ResultProduction::READY;
    }

    /// AVAILABLE -> RESERVED for one waiting consumer. False when the result is
    /// already reserved, consumed or expired; the consumer raises its own
    /// repeat-consumption error at that boundary.
    [[nodiscard]] bool reserve() noexcept {
        if (consumption_ != ResultConsumption::AVAILABLE) {
            return false;
        }
        consumption_ = ResultConsumption::RESERVED;
        return true;
    }

    /// RESERVED -> AVAILABLE: a cancelled wait gives the reservation back
    /// without consuming the result, so a later wait may still take it.
    void release_reservation() noexcept {
        assert(consumption_ == ResultConsumption::RESERVED && "releasing a reservation that is not held");
        consumption_ = ResultConsumption::AVAILABLE;
    }

    /// Commit the consumption. A consumer calls this *before* extracting the
    /// payload: a move that throws still ends the consumption, so a possibly
    /// half-moved value is never handed out again.
    void commit_consumed() noexcept {
        assert(consumable() && "consuming a result that was already consumed or expired");
        consumption_ = ResultConsumption::CONSUMED;
    }

    /// Release the captured error after consumption or expiry, before storage dies.
    void clear_error() noexcept { error_ = {}; }

    /// AVAILABLE/RESERVED -> EXPIRED. The owner drops the payload afterwards;
    /// an expired result is never consumable again.
    void expire() noexcept {
        assert(consumable() && "expiring a result that was already consumed or expired");
        consumption_ = ResultConsumption::EXPIRED;
    }

protected:
    /// PRODUCING -> WRITTEN, called by the derived write paths once the slot
    /// holds the outcome. Writing twice is a contract violation.
    void mark_written() noexcept {
        assert(production_ == ResultProduction::PRODUCING && "result written twice");
        production_ = ResultProduction::WRITTEN;
    }

private:
    /// A result may be consumed or expired, but only while it is still free.
    [[nodiscard]] bool consumable() const noexcept {
        return consumption_ == ResultConsumption::AVAILABLE || consumption_ == ResultConsumption::RESERVED;
    }

    ResultProduction production_ = ResultProduction::PRODUCING;
    ResultContent content_ = ResultContent::VALUE;
    ResultConsumption consumption_ = ResultConsumption::AVAILABLE;
    std::exception_ptr error_{};
};

/// Result slot for `T`, stored outside the frame that produces it.
///
/// The producer constructs the payload in place through write_value(), so
/// return_value() needs no slot of its own and never allocates control state.
template<typename T>
class ResultState : public ResultStateBase {
public:
    /// Construct the payload in place and mark the result written. The one
    /// path here that may throw: T's constructor runs in this call. A throw
    /// leaves the slot PRODUCING and empty, so the coroutine's exception
    /// handling writes the defect instead of publishing a success.
    template<typename... Args>
    void write_value(Args&&... args) {
        value_.emplace(std::forward<Args>(args)...);
        mark_written();
    }

    /// Move the payload out; the consumer has committed the consumption first.
    /// May throw T's move constructor — the consumption is final either way.
    [[nodiscard]] T take_value() {
        assert(consumption() == ResultConsumption::CONSUMED && "extracting before the consumption is committed");
        return std::move(*value_);
    }

    /// Release payload after its consumer/owner has revoked consumption rights.
    void clear_payload() noexcept {
        clear_error();
        value_.reset();
    }

private:
    std::optional<T> value_;
};

/// Result slot for a void task: no payload, only the success marker.
template<>
class ResultState<void> : public ResultStateBase {
public:
    /// Mark a completed body a success. Never throws.
    void write_success() noexcept { mark_written(); }

    void clear_payload() noexcept { clear_error(); }
};

}  // namespace coro::detail

#endif  // YADDNSC_CORO_DETAIL_RESULT_STATE_HPP
