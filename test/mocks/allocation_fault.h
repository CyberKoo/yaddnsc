//
// allocation_fault — binary-wide allocation fault injection for tests.
//
// Test binaries that need allocation faults link the test_allocation_fault
// object library, which carries the replaced global operator new/delete
// family, and arm a hook that decides per allocation whether to throw.
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

}  // namespace test

#endif  // YADDNSC_TEST_MOCKS_ALLOCATION_FAULT_H
