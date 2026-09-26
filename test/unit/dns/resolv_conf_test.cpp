//
// Unit tests for DNS::parse_resolv_conf — /etc/resolv.conf discovery.
// =============================================================================

#include "infrastructure/dns/resolv_conf.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace {

class ResolvConfTest : public testing::Test {
protected:
    void TearDown() override {
        for (const auto& path : created_) {
            std::filesystem::remove(path);
        }
    }

    /// Write content to a unique temp file and return its path.
    std::filesystem::path write_temp(const std::string& name, const std::string& content) {
        const auto path = std::filesystem::temp_directory_path() / name;
        std::ofstream out(path);
        out << content;
        out.close();
        created_.push_back(path);
        return path;
    }

private:
    std::vector<std::filesystem::path> created_;
};

TEST_F(ResolvConfTest, MissingFile_ReturnsEmpty) {
    EXPECT_TRUE(DNS::parse_resolv_conf("/nonexistent/yaddnsc-resolv.conf").empty());
}

TEST_F(ResolvConfTest, ParsesNameservers) {
    const auto path = write_temp("yaddnsc-resolv-1.conf",
                                 "nameserver 223.5.5.5\n"
                                 "nameserver 2606:4700:4700::1111\n");
    const auto servers = DNS::parse_resolv_conf(path);
    ASSERT_EQ(servers.size(), 2U);
    EXPECT_EQ(servers[0].address, "223.5.5.5");
    EXPECT_EQ(servers[0].port, 53);
    EXPECT_EQ(servers[1].address, "2606:4700:4700::1111");
}

TEST_F(ResolvConfTest, IgnoresCommentsAndOtherDirectives) {
    const auto path = write_temp("yaddnsc-resolv-2.conf",
                                 "# a comment\n"
                                 "; another comment\n"
                                 "search example.com\n"
                                 "options ndots:5\n"
                                 "\n"
                                 "nameserver 1.1.1.1 # trailing comment\n"
                                 "nameserver 8.8.8.8 ; trailing comment\n");
    const auto servers = DNS::parse_resolv_conf(path);
    ASSERT_EQ(servers.size(), 2U);
    EXPECT_EQ(servers[0].address, "1.1.1.1");
    EXPECT_EQ(servers[1].address, "8.8.8.8");
}

TEST_F(ResolvConfTest, SkipsInvalidAddresses) {
    const auto path = write_temp("yaddnsc-resolv-3.conf",
                                 "nameserver not-an-ip\n"
                                 "nameserver\n"
                                 "nameserver 999.1.2.3\n"
                                 "nameserver 8.8.4.4\n");
    const auto servers = DNS::parse_resolv_conf(path);
    ASSERT_EQ(servers.size(), 1U);
    EXPECT_EQ(servers[0].address, "8.8.4.4");
}

}  // namespace
