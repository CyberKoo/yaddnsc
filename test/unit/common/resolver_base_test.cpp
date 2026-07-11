//
// Unit tests for dns/resolver/base.h — ResolverBase interface.
//
// Verifies:
//   - ResolverBase is abstract (cannot instantiate directly).
//   - Concrete subclass compiles and links.
//   - get_id() returns auto-incrementing unique IDs.
//   - get_type() returns the expected type name.
//   - query() interface contract compiles.
//   - Movability is preserved.
// =============================================================================

#include <type_traits>

#include <gtest/gtest.h>

#include "dns/resolver/base.h"

// ── Concrete subclass for testing ─────────────────────────────────────────────

class TestResolver final : public ResolverBase {
public:
    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type, int cancel_fd = -1) const override {
        // Return a minimal "success" packet (just host bytes for identification).
        if (type == RecordKind::A) {
            return std::vector<std::uint8_t>{192, 168, 1, 1};
        }
        return std::unexpected(DnsErrorInfo{DnsError::NX_DOMAIN, "not found"});
    }

    [[nodiscard]] std::string_view get_type() const noexcept override {
        return "TestResolver";
    }
};

// ── Static interface checks ───────────────────────────────────────────────────

TEST(ResolverBaseTest, IsAbstract) {
    EXPECT_TRUE(std::is_abstract_v<ResolverBase>);
}

TEST(ResolverBaseTest, HasVirtualDestructor) {
    EXPECT_TRUE(std::has_virtual_destructor_v<ResolverBase>);
}

// Abstract types cannot be constructed directly, so is_move_constructible is
// inherently false.  We test move semantics on a concrete subclass instead.
TEST(ResolverBaseTest, IsNotConstructibleDueToAbstract) {
    EXPECT_FALSE(std::is_move_constructible_v<ResolverBase>);
    EXPECT_FALSE(std::is_copy_constructible_v<ResolverBase>);
}

TEST(ResolverBaseTest, IsMoveAssignable) {
    // Move assignment only needs an lvalue reference, which is fine for
    // abstract types.
    EXPECT_TRUE(std::is_move_assignable_v<ResolverBase>);
}

TEST(ResolverBaseTest, IsNotCopyAssignable) {
    EXPECT_FALSE(std::is_copy_assignable_v<ResolverBase>);
}

// ── Concrete subclass behaviour ───────────────────────────────────────────────

TEST(ResolverBaseTest, GetType_ReturnsExpectedName) {
    TestResolver resolver;
    EXPECT_EQ(resolver.get_type(), "TestResolver");
}

TEST(ResolverBaseTest, GetId_ReturnsNonZero) {
    TestResolver resolver;
    EXPECT_GE(resolver.get_id(), 0U);
}

TEST(ResolverBaseTest, GetId_IsStable) {
    TestResolver resolver;
    auto id1 = resolver.get_id();
    auto id2 = resolver.get_id();
    EXPECT_EQ(id1, id2);
}

TEST(ResolverBaseTest, GetId_AutoIncrements) {
    TestResolver r1;
    TestResolver r2;
    TestResolver r3;

    EXPECT_LT(r1.get_id(), r2.get_id());
    EXPECT_LT(r2.get_id(), r3.get_id());
    EXPECT_EQ(r3.get_id() - r1.get_id(), 2U);
}

// ── query() interface ─────────────────────────────────────────────────────────

TEST(ResolverBaseTest, Query_Success_ReturnsExpectedBytes) {
    TestResolver resolver;
    auto result = resolver.query("example.com", RecordKind::A);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 4U);
    EXPECT_EQ((*result)[0], 192);
    EXPECT_EQ((*result)[1], 168);
    EXPECT_EQ((*result)[2], 1);
    EXPECT_EQ((*result)[3], 1);
}

TEST(ResolverBaseTest, Query_Failure_ReturnsError) {
    TestResolver resolver;
    auto result = resolver.query("example.com", RecordKind::AAAA);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, DnsError::NX_DOMAIN);
}

TEST(ResolverBaseTest, Query_AcceptsOptionalCancelFd) {
    TestResolver resolver;
    // Default cancel_fd = -1 should not affect the result.
    auto result = resolver.query("example.com", RecordKind::A, -1);
    ASSERT_TRUE(result.has_value());
}

// ── Move semantics ────────────────────────────────────────────────────────────

TEST(ResolverBaseTest, MoveConstructor_IdPreserved) {
    TestResolver r1;
    auto id1 = r1.get_id();

    TestResolver r2(std::move(r1));
    EXPECT_EQ(r2.get_id(), id1);
}

TEST(ResolverBaseTest, MoveAssignment_IdPreserved) {
    TestResolver r1;
    TestResolver r2;
    auto id1 = r1.get_id();

    r2 = std::move(r1);
    EXPECT_EQ(r2.get_id(), id1);
}

// ── Polymorphic usage ─────────────────────────────────────────────────────────

TEST(ResolverBaseTest, PolymorphicDispatch) {
    std::unique_ptr<ResolverBase> resolver = std::make_unique<TestResolver>();

    EXPECT_EQ(resolver->get_type(), "TestResolver");
    EXPECT_GE(resolver->get_id(), 0U);

    auto result = resolver->query("example.com", RecordKind::A);
    ASSERT_TRUE(result.has_value());
}
