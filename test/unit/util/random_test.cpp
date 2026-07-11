//
// Unit tests for util/random.hpp — Utils::Random::engine().
//
// Verifies:
//   - engine() returns a reference to a usable RNG engine.
//   - operator() produces values within the full uint32_t range.
//   - Multiple invocations produce different values (statistical
//     sanity — not a correctness proof, but catches trivial bugs).
//   - Multiple calls return the same thread-local engine (advancing
//     state is observable across calls).
// =============================================================================

#include <random>
#include <set>

#include <gtest/gtest.h>

#include "util/random.hpp"

TEST(RandomEngineTest, ReturnsReferenceToMt19937) {
    // Verify the return type is std::mt19937& (not a copy).
    auto& eng = Utils::Random::engine();
    using EngineType = std::mt19937;
    EXPECT_TRUE((std::is_same_v<decltype(eng), EngineType&>));
}

TEST(RandomEngineTest, ProducesValuesInRange) {
    auto& eng = Utils::Random::engine();
    // mt19937 produces values in [0, 2^32-1].
    for (int i = 0; i < 100; ++i) {
        auto val = eng();
        EXPECT_GE(val, 0U);
        EXPECT_LE(val, std::mt19937::max());
    }
}

TEST(RandomEngineTest, StateAdvancesAcrossCalls) {
    auto& eng = Utils::Random::engine();
    auto v1 = eng();
    auto v2 = eng();
    // Extremely unlikely that two consecutive draws are equal.
    EXPECT_NE(v1, v2);
}

TEST(RandomEngineTest, RepeatedCallsReturnSameEngine) {
    // The engine should be thread-local; calling twice from the same
    // thread should return the same object (advancing internal state).
    auto& eng1 = Utils::Random::engine();
    auto& eng2 = Utils::Random::engine();

    EXPECT_EQ(&eng1, &eng2);

    // State is shared — advancing eng1 should be visible via eng2.
    auto v1 = eng1();
    auto v2 = eng2();
    // If eng1 and eng2 are the same engine, eng2() should produce the
    // value *after* eng1(), so they should differ.
    EXPECT_NE(v1, v2);
}

TEST(RandomEngineTest, ReasonableDistribution) {
    // Quick sanity: produce 1000 values and verify they span the range.
    auto& eng = Utils::Random::engine();
    std::set<std::uint32_t> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(eng());
    }
    // With 1000 draws from a 32-bit space, virtually all values are
    // unique, but we just check at least 990 to avoid flakiness.
    EXPECT_GE(seen.size(), 990U);
}
