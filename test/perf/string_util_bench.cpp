//
// Benchmarks for string utility functions (split, trim, join, replace, etc.).
// =============================================================================

#include <benchmark/benchmark.h>

#include <string>
#include <string_view>
#include <vector>

#include "string_util.hpp"

// =============================================================================
// split / join
// =============================================================================

static void BM_StrSplitFew(benchmark::State &state) {
    constexpr std::string_view input = "a,b,c,d";
    for (auto _ : state) {
        auto parts = StringUtil::split(input, ",");
        benchmark::DoNotOptimize(parts);
    }
}
BENCHMARK(BM_StrSplitFew);

static void BM_StrSplitMany(benchmark::State &state) {
    std::string input;
    for (int i = 0; i < 100; ++i) {
        input += std::to_string(i) + ",";
    }
    input.pop_back();  // trailing comma
    for (auto _ : state) {
        auto parts = StringUtil::split(input, ",");
        benchmark::DoNotOptimize(parts);
    }
}
BENCHMARK(BM_StrSplitMany);

static void BM_StrJoinFew(benchmark::State &state) {
    std::vector<std::string> parts = {"a", "b", "c", "d"};
    for (auto _ : state) {
        auto result = StringUtil::join(parts, ",");
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_StrJoinFew);

// =============================================================================
// trim
// =============================================================================

static void BM_StrTrim(benchmark::State &state) {
    constexpr std::string_view input = "  \t  hello world  \n  ";
    for (auto _ : state) {
        auto result = StringUtil::trim(input);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_StrTrim);

// =============================================================================
// replace_all
// =============================================================================

static void BM_StrReplaceAll(benchmark::State &state) {
    std::string input = "the quick brown fox jumps over the lazy dog";
    for (auto _ : state) {
        auto s = input;
        StringUtil::replace_all(s, "the", "a");
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_StrReplaceAll);

// =============================================================================
// to_lower_copy / to_upper_copy
// =============================================================================

static void BM_StrToLower(benchmark::State &state) {
    constexpr std::string_view input = "HELLO WORLD! THIS IS A TEST STRING.";
    for (auto _ : state) {
        auto result = StringUtil::to_lower_copy(input);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_StrToLower);

static void BM_StrToUpper(benchmark::State &state) {
    constexpr std::string_view input = "hello world! this is a test string.";
    for (auto _ : state) {
        auto result = StringUtil::to_upper_copy(input);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_StrToUpper);

// =============================================================================
// iequals / icontains
// =============================================================================

static void BM_StrIEquals(benchmark::State &state) {
    constexpr std::string_view a = "Content-Type";
    constexpr std::string_view b = "content-type";
    for (auto _ : state) {
        auto result = StringUtil::iequals(a, b);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_StrIEquals);

static void BM_StrIContains(benchmark::State &state) {
    constexpr std::string_view haystack = "The Quick Brown Fox Jumps Over The Lazy Dog";
    constexpr std::string_view needle = "fox";
    for (auto _ : state) {
        auto result = StringUtil::icontains(haystack, needle);
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_StrIContains);

// =============================================================================
// reverse
// =============================================================================

static void BM_StrReverse(benchmark::State &state) {
    constexpr std::string_view input = "hello world! this is a test string to reverse.";
    for (auto _ : state) {
        auto s = std::string(input);
        StringUtil::reverse(s);
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_StrReverse);
