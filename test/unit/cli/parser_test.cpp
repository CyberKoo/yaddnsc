//
// Unit tests for the CLI parser (src/cli/parser.*): routing, aliases, options.
// Pure parsing — no I/O beyond temp config files, no dispatch.
// =============================================================================

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "cli/command.h"
#include "cli/parser.h"
#include "fixtures/cli_test_support.h"
#include "fixtures/sample_config.h"

using namespace CliTestSupport;

TEST(CliParseTest, NoArgs_ReturnsFailureWithoutCommand) {
    const auto result = parse({"yaddnsc"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_NE(result.exit_code, 0);
}

TEST(CliParseTest, VersionFlag_ExitsZero) {
    const auto result = parse({"yaddnsc", "--version"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_EQ(result.exit_code, 0);
}

TEST(CliParseTest, ShortVersionFlag_ExitsZero) {
    const auto result = parse({"yaddnsc", "-v"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_EQ(result.exit_code, 0);
}

TEST(CliParseTest, HelpFlag_ExitsZero) {
    const auto result = parse({"yaddnsc", "--help"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_EQ(result.exit_code, 0);
}

TEST(CliParseTest, UnknownSubcommand_Fails) {
    const auto result = parse({"yaddnsc", "frobnicate"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_NE(result.exit_code, 0);
}

TEST(CliParseTest, Run_DefaultConfig) {
    const auto result = parse({"yaddnsc", "run"});

    ASSERT_TRUE(result.command.has_value());
    const auto& cmd = std::get<Cli::RunCommand>(*result.command);
    EXPECT_EQ(cmd.config_path, "config.json");
    EXPECT_FALSE(cmd.verbose);
}

TEST(CliParseTest, Run_DebugFlag) {
    const auto result = parse({"yaddnsc", "run", "-d"});

    ASSERT_TRUE(result.command.has_value());
    EXPECT_TRUE(std::get<Cli::RunCommand>(*result.command).verbose);
}

TEST(CliParseTest, Run_LongDebugFlag) {
    const auto result = parse({"yaddnsc", "run", "--debug"});

    ASSERT_TRUE(result.command.has_value());
    EXPECT_TRUE(std::get<Cli::RunCommand>(*result.command).verbose);
}

TEST(CliParseTest, Run_WithConfigPath) {
    TempConfigFile cfg{std::string(Fixtures::MINIMAL_CONFIG)};
    const auto result = parse({"yaddnsc", "run", "-c", cfg.path()});

    ASSERT_TRUE(result.command.has_value());
    EXPECT_EQ(std::get<Cli::RunCommand>(*result.command).config_path, cfg.path());
}

TEST(CliParseTest, Run_NonExistentConfig_Fails) {
    const auto result = parse({"yaddnsc", "run", "-c", "/nonexistent/yaddnsc_config.json"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_NE(result.exit_code, 0);
}

TEST(CliParseTest, Dns_WithoutSubcommand_Fails) {
    const auto result = parse({"yaddnsc", "dns"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_NE(result.exit_code, 0);
}

TEST(CliParseTest, Driver_WithoutSubcommand_Fails) {
    const auto result = parse({"yaddnsc", "driver"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_NE(result.exit_code, 0);
}

TEST(CliParseTest, DriverList_Parses) {
    TempConfigFile cfg{std::string(Fixtures::MINIMAL_CONFIG)};
    const auto result = parse({"yaddnsc", "driver", "list", "-c", cfg.path()});

    ASSERT_TRUE(result.command.has_value());
    EXPECT_EQ(std::get<Cli::DriverListCommand>(*result.command).config_path, cfg.path());
}

TEST(CliParseTest, DriverInfo_ParsesName) {
    const auto result = parse({"yaddnsc", "driver", "info", "cloudflare"});

    ASSERT_TRUE(result.command.has_value());
    const auto& cmd = std::get<Cli::DriverInfoCommand>(*result.command);
    EXPECT_EQ(cmd.name, "cloudflare");
    EXPECT_EQ(cmd.config_path, "config.json");
}

TEST(CliParseTest, DriverInfo_RequiresName) {
    const auto result = parse({"yaddnsc", "driver", "info"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_NE(result.exit_code, 0);
}

TEST(CliParseTest, InterfaceAliases_ParseToSameCommand) {
    for (const auto& alias : {"interface", "if", "net"}) {
        const auto result = parse({"yaddnsc", alias, "list"});
        ASSERT_TRUE(result.command.has_value()) << "alias: " << alias;
        EXPECT_TRUE(std::holds_alternative<Cli::InterfaceListCommand>(*result.command)) << "alias: " << alias;
    }
}

TEST(CliParseTest, InterfaceIp_ParsesName) {
    const auto result = parse({"yaddnsc", "interface", "ip", "eth0"});

    ASSERT_TRUE(result.command.has_value());
    EXPECT_EQ(std::get<Cli::InterfaceIpCommand>(*result.command).name, "eth0");
}

TEST(CliParseTest, DnsResolve_ParsesHostAndDefaultType) {
    const auto result = parse({"yaddnsc", "dns", "resolve", "example.com"});

    ASSERT_TRUE(result.command.has_value());
    const auto& cmd = std::get<Cli::DnsResolveCommand>(*result.command);
    EXPECT_EQ(cmd.host, "example.com");
    EXPECT_EQ(cmd.type, "A");
}

TEST(CliParseTest, DnsResolve_AliasAndType) {
    const auto result = parse({"yaddnsc", "dns", "r", "example.com", "--type", "AAAA"});

    ASSERT_TRUE(result.command.has_value());
    const auto& cmd = std::get<Cli::DnsResolveCommand>(*result.command);
    EXPECT_EQ(cmd.host, "example.com");
    EXPECT_EQ(cmd.type, "AAAA");
}

TEST(CliParseTest, DnsResolve_InvalidType_Fails) {
    const auto result = parse({"yaddnsc", "dns", "resolve", "example.com", "--type", "BOGUS"});

    EXPECT_FALSE(result.command.has_value());
    EXPECT_NE(result.exit_code, 0);
}

TEST(CliParseTest, DnsResolver_Parses) {
    const auto result = parse({"yaddnsc", "dns", "resolver"});

    ASSERT_TRUE(result.command.has_value());
    EXPECT_TRUE(std::holds_alternative<Cli::DnsResolverCommand>(*result.command));
}

TEST(CliParseTest, ConfigShow_AliasParses) {
    for (const auto& alias : {"show", "s"}) {
        const auto result = parse({"yaddnsc", "config", alias});
        ASSERT_TRUE(result.command.has_value()) << "alias: " << alias;
        EXPECT_TRUE(std::holds_alternative<Cli::ConfigShowCommand>(*result.command)) << "alias: " << alias;
    }
}

TEST(CliParseTest, ConfigTest_AliasAndQuietFlag) {
    const auto result = parse({"yaddnsc", "config", "t", "-q"});

    ASSERT_TRUE(result.command.has_value());
    const auto& cmd = std::get<Cli::ConfigTestCommand>(*result.command);
    EXPECT_TRUE(cmd.quiet);
    EXPECT_EQ(cmd.config_path, "config.json");
}

TEST(CliParseTest, ConfigTest_LongQuietFlag) {
    const auto result = parse({"yaddnsc", "config", "test", "--quiet"});

    ASSERT_TRUE(result.command.has_value());
    EXPECT_TRUE(std::get<Cli::ConfigTestCommand>(*result.command).quiet);
}

TEST(CliParseTest, Info_Parses) {
    const auto result = parse({"yaddnsc", "info"});

    ASSERT_TRUE(result.command.has_value());
    EXPECT_TRUE(std::holds_alternative<Cli::InfoCommand>(*result.command));
}

// Locked CLI behaviour: -v/--version prints "yaddnsc/<version>" and exits zero.
TEST(CliParseTest, VersionFlag_PrintsProgramAndVersion) {
    StdoutCapture capture;
    const auto result = parse({"yaddnsc", "--version"});
    const std::string out = capture.str();

    EXPECT_EQ(result.exit_code, EXIT_SUCCESS);
    EXPECT_NE(out.find("yaddnsc/"), std::string::npos);
}
