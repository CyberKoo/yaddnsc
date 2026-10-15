#include <cstddef>
#include <initializer_list>
#include <new>
#include <optional>
#include <string>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/xml_raii.hpp>

#include "mocks/allocation_fault.h"

namespace {
thread_local bool fail_next_new = false;

void allocation_hook(std::size_t /*size*/) {
    if (fail_next_new) {
        fail_next_new = false;
        throw std::bad_alloc{};
    }
}

// Arm this binary's allocation-fault hook before any test runs.
[[maybe_unused]] const bool allocation_hook_armed = [] {
    test::set_allocation_fault_hook(&allocation_hook);
    return true;
}();
}  // namespace

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
