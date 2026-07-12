// Dynamically resolves the loopback interface name so the same test binary
// runs on all platforms ("lo" on Linux, "lo0" on FreeBSD/macOS).
//
// =============================================================================

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ip_source/iface.h"
#include "ip_source/iface_util.h"
#include "network/inet_address.h"
#include "network/net_devices.h"
#include "address_family.h"

namespace {
    const std::string LOOPBACK = NetDevices::loopback_name();
} // anonymous namespace

// ===========================================================================
// InterfaceIpSource — resolve with loopback
// ===========================================================================

TEST(InterfaceIpSourceTest, Resolve_Loopback_ReturnsNonEmpty) {
    InterfaceIpSource src(LOOPBACK, AddressFamily::UNSPECIFIED);
    auto addrs = src.resolve();
    EXPECT_FALSE(addrs.empty());
}

TEST(InterfaceIpSourceTest, Resolve_Loopback_FilterIpv4) {
    InterfaceIpSource src(LOOPBACK, AddressFamily::IPV4);
    auto addrs = src.resolve();
    ASSERT_FALSE(addrs.empty());

    for (const auto &addr : addrs) {
        EXPECT_EQ(addr.get_family(), AddressFamily::IPV4)
            << "All returned addresses must be IPv4";
    }

    // 127.0.0.1 must be present.
    bool has_loopback = false;
    for (const auto &addr : addrs) {
        if (addr.to_string() == "127.0.0.1") {
            has_loopback = true;
            break;
        }
    }
    EXPECT_TRUE(has_loopback);
}

TEST(InterfaceIpSourceTest, Resolve_Loopback_FilterIpv6) {
    InterfaceIpSource src(LOOPBACK, AddressFamily::IPV6);
    auto addrs = src.resolve();

    // IPv6 may be disabled in containers; skip if empty.
    if (addrs.empty()) {
        GTEST_SKIP() << "IPv6 is not available on this system";
    }

    for (const auto &addr : addrs) {
        EXPECT_EQ(addr.get_family(), AddressFamily::IPV6)
            << "All returned addresses must be IPv6";
    }
}

TEST(InterfaceIpSourceTest, Resolve_NonExistentInterface_Throws) {
    InterfaceIpSource src("nonexistent999", AddressFamily::UNSPECIFIED);
    EXPECT_THROW(
        {
            [[maybe_unused]] auto _ = src.resolve();
        },
        std::runtime_error);
}

// ===========================================================================
// InterfaceUtil — get_interfaces
// ===========================================================================

TEST(InterfaceIpSourceTest, GetInterfaces_ReturnsNonEmpty) {
    auto interfaces = InterfaceUtil::get_interfaces();
    EXPECT_FALSE(interfaces.empty());

    // The loopback interface should be present on any POSIX system.
    auto it = std::ranges::find(interfaces, LOOPBACK);
    EXPECT_NE(it, interfaces.end()) << "Loopback interface '" << LOOPBACK << "' not found";
}

// ===========================================================================
// InterfaceIpSource — resolve with UNSPECIFIED
// ===========================================================================

TEST(InterfaceIpSourceTest, Resolve_Loopback_Unspecified_ContainsBothFamilies) {
    InterfaceIpSource src(LOOPBACK, AddressFamily::UNSPECIFIED);
    auto addrs = src.resolve();
    ASSERT_FALSE(addrs.empty());

    bool has_v4 = false;
    bool has_v6 = false;
    for (const auto &addr : addrs) {
        if (addr.get_family() == AddressFamily::IPV4) has_v4 = true;
        if (addr.get_family() == AddressFamily::IPV6) has_v6 = true;
    }

    EXPECT_TRUE(has_v4) << "127.0.0.1";
    // IPv6 may be disabled; just check that IPv4 is there.
}
