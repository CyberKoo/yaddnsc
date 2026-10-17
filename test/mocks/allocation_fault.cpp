//
// allocation_fault — replaced global operator new/delete family, the
// armed-hook dispatch and the count-only mode. See mocks/allocation_fault.h
// for the API.
//
// The replacement family lives in its own translation unit by design:
//  - ASan consistency: a malloc-backed operator new paired with the default
//    operator delete trips alloc-dealloc-mismatch at runtime, so the whole
//    family must be replaced together with the malloc/free pairing intact.
//  - GCC -Wmismatched-new-delete: defined next to test code, operator delete
//    gets inlined into new-expression cleanups (e.g. gtest's CreateTest) and
//    free() on a pointer GCC attributes to operator new fails -Werror builds.
//    This TU contains no new-expression, so there is nothing to misjudge.
// =============================================================================

#include "mocks/allocation_fault.h"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<test::AllocationFaultHook> armed_hook{nullptr};
std::atomic<bool> count_allocations{false};
std::atomic<std::size_t> allocations{0};

}  // namespace

namespace test {

void set_allocation_fault_hook(AllocationFaultHook hook) noexcept {
    armed_hook.store(hook, std::memory_order_relaxed);
}

AllocationCounter::AllocationCounter() noexcept {
    allocations.store(0, std::memory_order_relaxed);
    count_allocations.store(true, std::memory_order_relaxed);
}

AllocationCounter::~AllocationCounter() noexcept {
    count_allocations.store(false, std::memory_order_relaxed);
}

std::size_t AllocationCounter::count() noexcept {
    return allocations.load(std::memory_order_relaxed);
}

}  // namespace test

// Replacement allocators require the matching malloc/free ownership boundary.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

namespace {

void* allocate(std::size_t size) {
    if (const auto hook = armed_hook.load(std::memory_order_relaxed); hook != nullptr) {
        hook(size);
    }
    if (count_allocations.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* result = std::malloc(size == 0 ? 1 : size)) {
        return result;
    }
    throw std::bad_alloc{};
}

}  // namespace

// Test-only replacement allocation functions, paired with malloc/free.
void* operator new(std::size_t size) {
    return allocate(size);
}

void* operator new[](std::size_t size) {
    return allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
