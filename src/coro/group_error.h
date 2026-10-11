// Coroutine runtime — supervised failures that nobody consumed.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_GROUP_ERROR_H
#define YADDNSC_INFRASTRUCTURE_CORO_GROUP_ERROR_H

#include <exception>
#include <utility>
#include <vector>

namespace coro {

/// A supervisor leaves siblings running, then reports every unobserved child
/// defect at scope exit. Failures consumed through Handle/next are not repeated.
/// Owns exception_ptrs; cancellation is never included. Thread safety: immutable
/// after construction. Allocation may throw while collecting the failures.
class GroupError final : public std::exception {
public:
    explicit GroupError(std::vector<std::exception_ptr> errors) noexcept : errors_(std::move(errors)) {}

    [[nodiscard]] const char* what() const noexcept override { return "unobserved supervised child failures"; }

    [[nodiscard]] const std::vector<std::exception_ptr>& errors() const noexcept { return errors_; }

private:
    std::vector<std::exception_ptr> errors_;
};

}  // namespace coro
#endif  // YADDNSC_INFRASTRUCTURE_CORO_GROUP_ERROR_H
