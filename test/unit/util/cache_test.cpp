//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for util/cache.hpp — Utils::Cache::TtlCache.
//
// Verifies:
//   - get/set/contains/remove/clear basic operations.
//   - TTL expiry.
//   - get_or_compute with thundering-herd protection.
//   - Thread safety under concurrent access.
// =============================================================================

#include <thread>
#include <chrono>
#include <atomic>
#include <vector>

#include <gtest/gtest.h>

#include "util/cache.hpp"

using namespace std::chrono_literals;

// ── Basic operations ─────────────────────────────────────────────────────────

TEST(TtlCacheTest, Default_Empty) {
    Utils::Cache::TtlCache<int, std::string> cache(1h);
    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(cache.size(), 0U);
}

TEST(TtlCacheTest, SetAndGet) {
    Utils::Cache::TtlCache<int, std::string> cache(1h);
    cache.set(1, "one");
    cache.set(2, "two");

    EXPECT_FALSE(cache.empty());
    EXPECT_EQ(cache.size(), 2U);

    auto v1 = cache.get(1);
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, "one");

    auto v2 = cache.get(2);
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, "two");
}

TEST(TtlCacheTest, Get_MissingKey_ReturnsNullopt) {
    Utils::Cache::TtlCache<int, std::string> cache(1h);
    auto v = cache.get(42);
    EXPECT_FALSE(v.has_value());
}

TEST(TtlCacheTest, Contains_ExistingKey) {
    Utils::Cache::TtlCache<int, std::string> cache(1h);
    cache.set(1, "one");
    EXPECT_TRUE(cache.contains(1));
    EXPECT_FALSE(cache.contains(2));
}

TEST(TtlCacheTest, Remove_ExistingKey) {
    Utils::Cache::TtlCache<int, std::string> cache(1h);
    cache.set(1, "one");
    EXPECT_TRUE(cache.contains(1));

    cache.remove(1);
    EXPECT_FALSE(cache.contains(1));
    EXPECT_TRUE(cache.empty());
}

TEST(TtlCacheTest, Clear_RemovesAll) {
    Utils::Cache::TtlCache<int, std::string> cache(1h);
    cache.set(1, "one");
    cache.set(2, "two");
    EXPECT_EQ(cache.size(), 2U);

    cache.clear();
    EXPECT_TRUE(cache.empty());
}

// ── TTL expiry ───────────────────────────────────────────────────────────────

TEST(TtlCacheTest, Get_ExpiredEntry_ReturnsNullopt) {
    // TTL of 0 means every entry is immediately expired.
    Utils::Cache::TtlCache<int, std::string> cache(0ns);
    cache.set(1, "one");

    // The entry should be expired immediately.
    auto v = cache.get(1);
    EXPECT_FALSE(v.has_value());
    EXPECT_TRUE(cache.empty());
}

TEST(TtlCacheTest, Contains_ExpiredEntry_ReturnsFalse) {
    Utils::Cache::TtlCache<int, std::string> cache(0ns);
    cache.set(1, "one");
    EXPECT_FALSE(cache.contains(1));
}

// ── get_or_compute ───────────────────────────────────────────────────────────

TEST(TtlCacheTest, GetOrCompute_Miss_ComputesAndCaches) {
    Utils::Cache::TtlCache<int, std::string> cache(1h);
    int compute_count = 0;

    auto v = cache.get_or_compute(1, [&] {
        ++compute_count;
        return std::string("computed");
    });

    EXPECT_EQ(v, "computed");
    EXPECT_EQ(compute_count, 1);

    // Second call should use cache.
    auto v2 = cache.get_or_compute(1, [&] {
        ++compute_count;
        return std::string("should not be called");
    });

    EXPECT_EQ(v2, "computed");
    EXPECT_EQ(compute_count, 1);  // factory not called again
}

TEST(TtlCacheTest, GetOrCompute_Expired_Recomputes) {
    Utils::Cache::TtlCache<int, std::string> cache(0ns);
    int compute_count = 0;

    cache.get_or_compute(1, [&] {
        ++compute_count;
        return std::string("first");
    });

    auto v = cache.get_or_compute(1, [&] {
        ++compute_count;
        return std::string("second");
    });

    EXPECT_EQ(v, "second");
    EXPECT_EQ(compute_count, 2);  // was expired, so recomputed
}

TEST(TtlCacheTest, GetOrCompute_FactoryThrows_PropagatesException) {
    Utils::Cache::TtlCache<int, std::string> cache(1h);

    // Factory throws an exception — should propagate to caller
    EXPECT_THROW(
        {
            cache.get_or_compute(1, [&]() -> std::string {
                throw std::runtime_error("factory failure");
            });
        },
        std::runtime_error
    );

    // After failure, the key should NOT be cached
    EXPECT_FALSE(cache.contains(1));
}

// ── Thread safety ────────────────────────────────────────────────────────────

TEST(TtlCacheTest, ConcurrentGetOrCompute_SingleKey) {
    Utils::Cache::TtlCache<int, int> cache(1h);
    std::atomic<int> compute_count{0};
    // Synchronisation: all threads wait until the main thread sets the flag.
    // This ensures all threads arrive at the cache at roughly the same time.
    std::promise<void> go;
    auto shared_go = go.get_future().share();

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, shared_go] {
            shared_go.wait();  // wait for the start signal
            auto v = cache.get_or_compute(42, [&] {
                compute_count.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(50ms);  // force contention
                return 100;
            });
            EXPECT_EQ(v, 100);
        });
    }

    // Give all threads time to reach the barrier, then signal them
    std::this_thread::sleep_for(5ms);
    go.set_value();

    for (auto &t : threads) {
        t.join();
    }

    // Only one thread should have actually computed.
    EXPECT_EQ(compute_count.load(), 1);
}

// ── invalidate_if ────────────────────────────────────────────────────────────

TEST(TtlCacheTest, InvalidateIf_RemovesMatching) {
    Utils::Cache::TtlCache<int, int> cache(1h);
    cache.set(1, 11);  // odd  — kept
    cache.set(2, 20);  // even — removed
    cache.set(3, 31);  // odd  — kept
    cache.set(4, 40);  // even — removed

    // Invalidate entries with even values.
    cache.invalidate_if([](int /*key*/, int value) { return value % 2 == 0; });

    EXPECT_TRUE(cache.contains(1));
    EXPECT_FALSE(cache.contains(2));
    EXPECT_TRUE(cache.contains(3));
    EXPECT_FALSE(cache.contains(4));
}
