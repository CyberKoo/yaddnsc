#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/xml_raii.hpp>

namespace {
thread_local bool fail_next_new = false;
int xml_allocations = 0;

void* tracked_malloc(std::size_t size) {
    void* result = std::malloc(size);
    if (result)
        ++xml_allocations;
    return result;
}

void tracked_free(void* pointer) {
    if (pointer)
        --xml_allocations;
    std::free(pointer);
}

void* tracked_realloc(void* pointer, std::size_t size) {
    if (!pointer)
        return tracked_malloc(size);
    if (size == 0) {
        tracked_free(pointer);
        return nullptr;
    }
    return std::realloc(pointer, size);
}

char* tracked_strdup(const char* value) {
    const auto size = std::strlen(value) + 1;
    auto* result = static_cast<char*>(tracked_malloc(size));
    if (result)
        std::memcpy(result, value, size);
    return result;
}

class XmlAllocatorScope {
public:
    XmlAllocatorScope() {
        xmlMemGet(&free_, &malloc_, &realloc_, &strdup_);
        xmlMemSetup(&tracked_free, &tracked_malloc, &tracked_realloc, &tracked_strdup);
    }

    ~XmlAllocatorScope() noexcept {
        xmlCleanupParser();
        xmlMemSetup(free_, malloc_, realloc_, strdup_);
    }

    XmlAllocatorScope(const XmlAllocatorScope&) = delete;
    XmlAllocatorScope& operator=(const XmlAllocatorScope&) = delete;

private:
    xmlFreeFunc free_{};
    xmlMallocFunc malloc_{};
    xmlReallocFunc realloc_{};
    xmlStrdupFunc strdup_{};
};
}  // namespace

// This binary owns an isolated allocator fault; production code has no hook.
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

TEST(XmlRaiiAllocation, TextBuffersAreFreedWhenStringOrVectorAllocationThrows) {
    const XmlAllocatorScope allocator;
    const auto text = std::string(400, 'x');
    auto document = xml_raii::XmlDocument::parse("<root><item>" + text + "</item></root>");
    ASSERT_TRUE(document);
    // Warm libxml's XPath caches before measuring transient resources.
    ASSERT_TRUE(document->first_text("/root/item"));
    const int baseline = xml_allocations;
    ASSERT_GT(baseline, 0) << "libxml allocation hooks must be active for this regression check";
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
        EXPECT_EQ(xml_allocations, baseline);
    }
}
