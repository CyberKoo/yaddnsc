//
// Benchmarks for Inet4Address, Inet6Address, and InetAddress parsing/serialization.
// =============================================================================

#include <benchmark/benchmark.h>

#include <string>
#include <string_view>

#include "network/inet_address.h"

// =============================================================================
// Inet4Address
// =============================================================================

static void BM_Inet4Parse(benchmark::State &state) {
    constexpr std::string_view addr = "192.168.1.1";
    for (auto _ : state) {
        auto parsed = Inet4Address::parse(addr);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_Inet4Parse);

static void BM_Inet4ToString(benchmark::State &state) {
    auto addr = Inet4Address::parse("192.168.1.1").value();
    for (auto _ : state) {
        auto s = addr.to_string();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Inet4ToString);

static void BM_Inet4Classification(benchmark::State &state) {
    auto addr = Inet4Address::parse("127.0.0.1").value();
    for (auto _ : state) {
        benchmark::DoNotOptimize(addr.is_loopback());
        benchmark::DoNotOptimize(addr.is_multicast());
        benchmark::DoNotOptimize(addr.is_unspecified());
    }
}
BENCHMARK(BM_Inet4Classification);

// =============================================================================
// Inet6Address
// =============================================================================

static void BM_Inet6Parse(benchmark::State &state) {
    constexpr std::string_view addr = "2001:db8::1";
    for (auto _ : state) {
        auto parsed = Inet6Address::parse(addr);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_Inet6Parse);

static void BM_Inet6ParseFull(benchmark::State &state) {
    constexpr std::string_view addr = "2001:0db8:85a3:0000:0000:8a2e:0370:7334";
    for (auto _ : state) {
        auto parsed = Inet6Address::parse(addr);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_Inet6ParseFull);

static void BM_Inet6ToString(benchmark::State &state) {
    auto addr = Inet6Address::parse("2001:db8::1").value();
    for (auto _ : state) {
        auto s = addr.to_string();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_Inet6ToString);

// =============================================================================
// InetAddress (type-erased)
// =============================================================================

static void BM_InetAddressParseV4(benchmark::State &state) {
    constexpr std::string_view addr = "10.0.0.1";
    for (auto _ : state) {
        auto parsed = InetAddress::parse(addr);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_InetAddressParseV4);

static void BM_InetAddressParseV6(benchmark::State &state) {
    constexpr std::string_view addr = "fe80::1";
    for (auto _ : state) {
        auto parsed = InetAddress::parse(addr);
        benchmark::DoNotOptimize(parsed);
    }
}
BENCHMARK(BM_InetAddressParseV6);

static void BM_InetAddressToString(benchmark::State &state) {
    auto addr = InetAddress::parse("192.168.1.1").value();
    for (auto _ : state) {
        auto s = addr.to_string();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_InetAddressToString);

static void BM_InetAddressVisit(benchmark::State &state) {
    auto addr = InetAddress::parse("192.168.1.1").value();
    for (auto _ : state) {
        addr.visit([](const auto &a) {
            benchmark::DoNotOptimize(a.get_family());
        });
    }
}
BENCHMARK(BM_InetAddressVisit);
