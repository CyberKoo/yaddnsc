//
// Benchmarks for SocketAddr construction and serialization.
// =============================================================================

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>
#include <string_view>

#include <netinet/in.h>
#include <sys/socket.h>

#include "network/inet_address.h"
#include "network/socket_addr.h"

// =============================================================================
// SocketAddr::from_inet
// =============================================================================

static void BM_SocketAddrFromInetV4(benchmark::State &state) {
    auto addr = InetAddress::parse("192.168.1.1").value();
    for (auto _ : state) {
        auto sa = SocketAddr::from_inet(addr, 53);
        benchmark::DoNotOptimize(sa);
    }
}
BENCHMARK(BM_SocketAddrFromInetV4);

static void BM_SocketAddrFromInetV6(benchmark::State &state) {
    auto addr = InetAddress::parse("2001:db8::1").value();
    for (auto _ : state) {
        auto sa = SocketAddr::from_inet(addr, 53);
        benchmark::DoNotOptimize(sa);
    }
}
BENCHMARK(BM_SocketAddrFromInetV6);

// =============================================================================
// SocketAddr accessors
// =============================================================================

static void BM_SocketAddrToStringV4(benchmark::State &state) {
    auto addr = InetAddress::parse("10.0.0.1").value();
    auto sa = SocketAddr::from_inet(addr, 8080).value();
    for (auto _ : state) {
        auto s = sa.to_string();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SocketAddrToStringV4);

static void BM_SocketAddrToStringV6(benchmark::State &state) {
    auto addr = InetAddress::parse("::1").value();
    auto sa = SocketAddr::from_inet(addr, 53).value();
    for (auto _ : state) {
        auto s = sa.to_string();
        benchmark::DoNotOptimize(s);
    }
}
BENCHMARK(BM_SocketAddrToStringV6);

static void BM_SocketAddrExtractAddress(benchmark::State &state) {
    auto addr = InetAddress::parse("192.168.1.1").value();
    auto sa = SocketAddr::from_inet(addr, 53).value();
    for (auto _ : state) {
        auto extracted = sa.address();
        benchmark::DoNotOptimize(extracted);
    }
}
BENCHMARK(BM_SocketAddrExtractAddress);

// =============================================================================
// SocketAddr::from_raw (simulating recvfrom path)
// =============================================================================

static void BM_SocketAddrFromRawV4(benchmark::State &state) {
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = __builtin_bswap16(53);
    sin.sin_addr.s_addr = __builtin_bswap32(0x08080808);  // 8.8.8.8
    for (auto _ : state) {
        auto sa = SocketAddr::from_raw(reinterpret_cast<const sockaddr *>(&sin), sizeof(sin));
        benchmark::DoNotOptimize(sa);
    }
}
BENCHMARK(BM_SocketAddrFromRawV4);
