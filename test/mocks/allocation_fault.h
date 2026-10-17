//
// allocation_fault — binary-wide allocation fault injection for tests.
//
// Test binaries that need allocation faults link the test_allocation_fault
// object library, which carries the replaced global operator new/delete
// family, and arm a hook that decides per allocation whether to throw.
// The same family also counts allocations without failing them, which is how
// benchmarks report allocations/op.
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_ALLOCATION_FAULT_H
#define YADDNSC_TEST_MOCKS_ALLOCATION_FAULT_H

#include <cstddef>

namespace test {

/// Called by this binary's replaced global operator new on every allocation.
/// Throwing from the hook injects that exception into the allocation
/// (throw std::bad_alloc for a plain out-of-memory fault); returning lets
/// the allocation proceed.
using AllocationFaultHook = void (*)(std::size_t size);

/// Arms the hook process-wide; nullptr clears it. Not thread-sensitive:
/// per-thread arming belongs in the hook's own thread_local state.
void set_allocation_fault_hook(AllocationFaultHook hook) noexcept;

/// Count-only mode of the same replaced allocator: records how many
/// allocations happened and lets every one of them succeed.
///
/// Independent of the fault hook — the two compose, so a binary can count
/// while it injects. Used to report allocations/op in benchmarks, where the
/// count is the measurement and failing would end the run.
///
/// One counter at a time: it is a single process-wide count, so scopes must
/// not overlap (an inner scope would zero the outer's total).
///
/// Thread safety: relaxed atomics, matching the hook's own contract; intended
/// for the thread that drives the measured region.
class AllocationCounter {
public:
    /// Zeroes the process-wide count and arms counting: every allocation from
    /// here on counts until this object dies.
    AllocationCounter() noexcept;

    /// Disarms counting.
    ~AllocationCounter() noexcept;

    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;

    /// Allocations that happened since this counter was constructed.
    [[nodiscard]] static std::size_t count() noexcept;
};

}  // namespace test

#endif  // YADDNSC_TEST_MOCKS_ALLOCATION_FAULT_H
