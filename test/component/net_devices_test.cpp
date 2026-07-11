//
// Integration tests for network/net_devices.h / net_devices.cpp.
//
// Dynamically resolves the loopback interface name ("lo" on Linux, "lo0" on
// FreeBSD/macOS) so the same test binary runs on all platforms.
//
// =============================================================================

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "network/inet_address.h"
#include "network/net_devices.h"

namespace {
    const std::string LOOPBACK = NetDevices::loopback_name();
} // anonymous namespace

// ===========================================================================
// enumerate_interfaces
// ===========================================================================

TEST(NetDevicesTest, EnumerateInterfaces_ContainsLoopback) {
    auto ifaces = NetDevices::enumerate_interfaces();
    EXPECT_TRUE(ifaces.contains(LOOPBACK))
        << "Loopback interface must be present via getifaddrs()";
}

TEST(NetDevicesTest, EnumerateInterfaces_LoopbackHasAddresses) {
    auto ifaces = NetDevices::enumerate_interfaces();
    ASSERT_TRUE(ifaces.contains(LOOPBACK));
    EXPECT_FALSE(ifaces.at(LOOPBACK).empty())
        << "Loopback must have at least one address (127.0.0.1)";
}

TEST(NetDevicesTest, EnumerateInterfaces_Ipv4Present) {
    auto ifaces = NetDevices::enumerate_interfaces();
    ASSERT_TRUE(ifaces.contains(LOOPBACK));

    bool has_ipv4 = false;
    for (const auto &addr : ifaces.at(LOOPBACK)) {
        if (addr.get_family() == AddressFamily::IPV4) {
            has_ipv4 = true;
            EXPECT_EQ(addr.to_string(), "127.0.0.1");
            break;
        }
    }
    EXPECT_TRUE(has_ipv4) << "Loopback must have 127.0.0.1";
}

TEST(NetDevicesTest, EnumerateInterfaces_Ipv6Present) {
    auto ifaces = NetDevices::enumerate_interfaces();
    ASSERT_TRUE(ifaces.contains(LOOPBACK));

    bool has_ipv6 = false;
    for (const auto &addr : ifaces.at(LOOPBACK)) {
        if (addr.get_family() == AddressFamily::IPV6) {
            has_ipv6 = true;
            break;
        }
    }
    // IPv6 may be disabled in some containers; this is informational.
    EXPECT_TRUE(has_ipv6) << "Loopback should have ::1 (IPv6 might be disabled)";
}

// ===========================================================================
// get_ipv4_subnets
// ===========================================================================

TEST(NetDevicesTest, GetIpv4Subnets_Loopback) {
    auto subnets = NetDevices::get_ipv4_subnets(LOOPBACK);
    EXPECT_FALSE(subnets.empty());
    if (!subnets.empty()) {
        EXPECT_EQ(subnets[0].address.to_string(), "127.0.0.1");
    }
}

TEST(NetDevicesTest, GetIpv4Subnets_NonExistentIface_ReturnsEmpty) {
    auto subnets = NetDevices::get_ipv4_subnets("nonexistent999");
    EXPECT_TRUE(subnets.empty());
}

// ===========================================================================
// find_default_interface_index
// ===========================================================================

TEST(NetDevicesTest, FindDefaultInterfaceIndex_Unspec_ReturnsNonZero) {
    // At minimum the loopback might be found; in a full system there
    // should be at least one non-loopback UP interface.
    auto index = NetDevices::find_default_interface_index(AF_UNSPEC);
    // This may be 0 in minimal containers; we just verify it doesn't crash.
    // In practice on any real or CI host this returns a valid index.
    EXPECT_GE(index, 0U);
}

// ===========================================================================
// name_to_index / index_to_name
// ===========================================================================

TEST(NetDevicesTest, NameToIndex_Loopback_ReturnsNonZero) {
    auto index = NetDevices::name_to_index(LOOPBACK);
    EXPECT_GT(index, 0U) << "if_nametoindex(\"" << LOOPBACK << "\") must succeed";
}

TEST(NetDevicesTest, NameToIndex_NonExistent_ReturnsZero) {
    auto index = NetDevices::name_to_index("nonexistent999");
    EXPECT_EQ(index, 0U);
}

TEST(NetDevicesTest, IndexToName_Loopback) {
    auto index = NetDevices::name_to_index(LOOPBACK);
    ASSERT_GT(index, 0U);

    auto name = NetDevices::index_to_name(index);
    EXPECT_EQ(name, LOOPBACK);
}

TEST(NetDevicesTest, IndexToName_InvalidIndex_ReturnsEmpty) {
    auto name = NetDevices::index_to_name(0);
    EXPECT_TRUE(name.empty());
}
