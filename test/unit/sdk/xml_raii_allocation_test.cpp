#include <cstdlib>
#include <new>
#include <string>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/xml_raii.hpp>

namespace {
thread_local bool fail_next_new = false;
}  // namespace

// This binary owns the global operator new to inject an isolated allocation fault.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
void* operator new(std::size_t size) {
    if (fail_next_new) {
        fail_next_new = false;
        throw std::bad_alloc();
    }
    if (void* result = std::malloc(size == 0 ? 1 : size))
        return result;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
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

// Leak-freedom on the throwing path is enforced by the sanitizer-instrumented
// CI targets: libxml2 deprecated its global allocator hooks, so LeakSanitizer
// reporting unreleased buffers at exit stands in for allocation counting.
TEST(XmlRaiiAllocation, TextBuffersAreFreedWhenStringOrVectorAllocationThrows) {
    const auto text = std::string(400, 'x');
    auto document = xml_raii::XmlDocument::parse("<root><item>" + text + "</item></root>");
    ASSERT_TRUE(document);
    for (bool all : {false, true}) {
        bool threw = false;
        fail_next_new = true;
        try {
            if (all) {
                [[maybe_unused]] auto result = document->all_text("/root/item");
            } else {
                [[maybe_unused]] auto result = document->first_text("/root/item");
            }
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        fail_next_new = false;
        EXPECT_TRUE(threw);
        const auto recovered = document->first_text("/root/item");
        ASSERT_TRUE(recovered);
        EXPECT_EQ(*recovered, text);
    }
}
