//
// Benchmarks for DNS wire-format QueryBuilder.
// =============================================================================

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dns/types.h"
#include "dns/wire/builder.h"

// =============================================================================
// QueryBuilder — simple single-question query
// =============================================================================

static void BM_QueryBuilderSingle(benchmark::State &state) {
    for (auto _ : state) {
        auto packet = DNS::QueryBuilder{}
            .add_question("example.com", DNS::RecordType::A)
            .build();
        benchmark::DoNotOptimize(packet);
    }
}
BENCHMARK(BM_QueryBuilderSingle);

// =============================================================================
// QueryBuilder — multiple questions
// =============================================================================

static void BM_QueryBuilderMultiQuestion(benchmark::State &state) {
    for (auto _ : state) {
        auto packet = DNS::QueryBuilder{}
            .add_question("example.com", DNS::RecordType::A)
            .add_question("example.com", DNS::RecordType::AAAA)
            .build();
        benchmark::DoNotOptimize(packet);
    }
}
BENCHMARK(BM_QueryBuilderMultiQuestion);

// =============================================================================
// QueryBuilder — with EDNS0
// =============================================================================

static void BM_QueryBuilderWithEdns(benchmark::State &state) {
    for (auto _ : state) {
        auto packet = DNS::QueryBuilder{}
            .add_question("example.com", DNS::RecordType::A)
            .add_edns(1232, 0, true)
            .build();
        benchmark::DoNotOptimize(packet);
    }
}
BENCHMARK(BM_QueryBuilderWithEdns);

// =============================================================================
// QueryBuilder — long domain name
// =============================================================================

static void BM_QueryBuilderLongName(benchmark::State &state) {
    constexpr std::string_view long_name =
        "a-very-long-subdomain-name.that-exceeds-the-typical-length."
        "example-with-many-labels.example.com";
    for (auto _ : state) {
        auto packet = DNS::QueryBuilder{}
            .add_question(long_name, DNS::RecordType::TXT)
            .build();
        benchmark::DoNotOptimize(packet);
    }
}
BENCHMARK(BM_QueryBuilderLongName);

// =============================================================================
// QueryBuilder — full custom configuration
// =============================================================================

static void BM_QueryBuilderFullConfig(benchmark::State &state) {
    for (auto _ : state) {
        auto packet = DNS::QueryBuilder{}
            .id(0x1234)
            .rd(true)
            .add_question("www.example.com", DNS::RecordType::A)
            .add_question("www.example.com", DNS::RecordType::AAAA)
            .add_edns(4096, 0, true)
            .build();
        benchmark::DoNotOptimize(packet);
    }
}
BENCHMARK(BM_QueryBuilderFullConfig);
