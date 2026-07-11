//
// Benchmarks for URI parsing, encoding, and decoding.
// =============================================================================

#include <benchmark/benchmark.h>

#include <string>
#include <string_view>

#include "string_util.hpp"
#include "uri.h"

// =============================================================================
// Uri::parse benchmarks — various URI shapes
// =============================================================================

static void BM_UriParseSimple(benchmark::State &state) {
    constexpr std::string_view uri = "https://example.com/path";
    for (auto _ : state) {
        auto u = Uri::parse(uri);
        benchmark::DoNotOptimize(u);
    }
}
BENCHMARK(BM_UriParseSimple);

static void BM_UriParseWithPort(benchmark::State &state) {
    constexpr std::string_view uri = "https://example.com:8443/api/v1/update";
    for (auto _ : state) {
        auto u = Uri::parse(uri);
        benchmark::DoNotOptimize(u);
    }
}
BENCHMARK(BM_UriParseWithPort);

static void BM_UriParseWithQuery(benchmark::State &state) {
    constexpr std::string_view uri = "https://example.com/path?key1=value1&key2=value2&key3=12345";
    for (auto _ : state) {
        auto u = Uri::parse(uri);
        benchmark::DoNotOptimize(u);
    }
}
BENCHMARK(BM_UriParseWithQuery);

static void BM_UriParseIPv6(benchmark::State &state) {
    constexpr std::string_view uri = "https://[2001:db8::1]:8080/path?q=test";
    for (auto _ : state) {
        auto u = Uri::parse(uri);
        benchmark::DoNotOptimize(u);
    }
}
BENCHMARK(BM_UriParseIPv6);

// =============================================================================
// Uri accessor benchmarks
// =============================================================================

static void BM_UriGetQueryParams(benchmark::State &state) {
    constexpr std::string_view uri = "https://example.com/path?name=John+Doe&age=30&city=New+York";
    auto u = Uri::parse(uri);
    for (auto _ : state) {
        auto params = u.get_query_params();
        benchmark::DoNotOptimize(params);
    }
}
BENCHMARK(BM_UriGetQueryParams);

static void BM_UriGetOrigin(benchmark::State &state) {
    auto u = Uri::parse("https://example.com:8443/path");
    for (auto _ : state) {
        auto origin = u.get_origin();
        benchmark::DoNotOptimize(origin);
    }
}
BENCHMARK(BM_UriGetOrigin);

// =============================================================================
// Percent-encoding benchmarks
// =============================================================================

static void BM_UriEncodeUnreserved(benchmark::State &state) {
    constexpr std::string_view input = "simple-hostname-local";
    for (auto _ : state) {
        auto encoded = Uri::url_encode(input);
        benchmark::DoNotOptimize(encoded);
    }
}
BENCHMARK(BM_UriEncodeUnreserved);

static void BM_UriEncodeMixed(benchmark::State &state) {
    constexpr std::string_view input = "hello world! @example.com/path?query=value&x=1";
    for (auto _ : state) {
        auto encoded = Uri::url_encode(input);
        benchmark::DoNotOptimize(encoded);
    }
}
BENCHMARK(BM_UriEncodeMixed);

static void BM_UriDecode(benchmark::State &state) {
    constexpr std::string_view input = "hello%20world%21%20%40example.com%2Fpath%3Fquery%3Dvalue";
    for (auto _ : state) {
        auto decoded = Uri::url_decode(input);
        benchmark::DoNotOptimize(decoded);
    }
}
BENCHMARK(BM_UriDecode);
