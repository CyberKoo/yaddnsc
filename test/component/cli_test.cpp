//
// Component tests for the CLI layer (src/cli/*).
//
// Covers:
//   - Cli::parse_and_dispatch: subcommand routing, --help/--version,
//     parse errors, run options (-c/-d).
//   - Subcommand executors: config show/test, driver list/info,
//     dns resolve/resolver, interface list/ip, info.
//
// Requires:
//   - A built driver .so (simple) at TEST_DRIVER_DIR (dlopen for the config
//     test executor and the driver subcommands).
//   - A stub resolver registered under the "" schema so that dns resolve
//     never touches the network.
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "cli/cli.h"
#include "cli/config.h"
#include "cli/dns.h"
#include "cli/driver.h"
#include "cli/info.h"
#include "cli/interface.h"

#include "config/config.h"
#include "config/dns_config.h"
#include "dns/dns_error_info.h"
#include "dns/factory.h"
#include "dns/resolver/base.h"
#include "dns/resolver_registry.h"
#include "ip_source/iface_util.h"
#include "record_kind.h"
#include "util/cancellation_token.hpp"

#include "fixtures/sample_config.h"

// ===========================================================================
//  Stub resolver — replaces the real "" (classic) resolver so dns resolve
//  runs entirely in-process.  Behaviour is switched per test.
// ===========================================================================

namespace {
    using StubResult = std::expected<std::vector<std::uint8_t>, DnsErrorInfo>;
    std::function<StubResult(RecordKind)> g_stub_behavior;

    class StubResolver final : public ResolverBase {
    public:
        std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
        query(const std::string &, RecordKind type, const Utils::CancellationToken &) const override {
            return g_stub_behavior(type);
        }

        [[nodiscard]] std::string_view get_type() const noexcept override { return "Stub"; }
    };

    [[maybe_unused]] DnsResolverRegistry::Registrar stub_registrar(
        "", [](const Config::DnsServer &) { return std::make_unique<StubResolver>(); });

    /// NOERROR response with zero answers (question: example.com A IN).
    [[nodiscard]] std::vector<std::uint8_t> empty_response() {
        return {
            0x12, 0x34,                            // ID
            0x81, 0x80,                            // flags: QR, RD, RA
            0x00, 0x01,                            // QDCOUNT
            0x00, 0x00,                            // ANCOUNT
            0x00, 0x00,                            // NSCOUNT
            0x00, 0x00,                            // ARCOUNT
            0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00,  // question name
            0x00, 0x01,                            // QTYPE A
            0x00, 0x01,                            // QCLASS IN
        };
    }
} // namespace

// ===========================================================================
//  Helpers — argv construction + temp config files
// ===========================================================================

namespace {

    struct Argv {
        std::vector<std::string> storage;
        std::vector<char *> ptrs;

        [[nodiscard]] int argc() const { return static_cast<int>(ptrs.size()); }

        [[nodiscard]] char **data() { return ptrs.data(); }
    };

    /// Build argv that stays alive for the duration of the call: the returned
    /// struct owns both the string storage and the char* pointers.
    [[nodiscard]] Argv make_argv(std::vector<std::string> args) {
        Argv argv;
        argv.storage = std::move(args);
        argv.ptrs.reserve(argv.storage.size());
        for (auto &arg: argv.storage) {
            argv.ptrs.push_back(arg.data());
        }
        return argv;
    }

    class TempConfigFile {
    public:
        explicit TempConfigFile(std::string content) : path_(make_unique_path()) {
            std::ofstream out(path_);
            out << content;
        }

        ~TempConfigFile() {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }

        TempConfigFile(const TempConfigFile &) = delete;
        TempConfigFile &operator=(const TempConfigFile &) = delete;

        [[nodiscard]] const std::string &path() const { return path_; }

    private:
        [[nodiscard]] static std::string make_unique_path() {
            static std::atomic<unsigned> counter{0};
            const auto path = std::filesystem::temp_directory_path() /
                              ("yaddnsc_cli_test_" + std::to_string(::getpid()) + "_" +
                               std::to_string(counter.fetch_add(1)) + ".json");
            return path.string();
        }

        std::string path_;
    };

    /// Config that loads the real "simple" driver from the build tree.
    [[nodiscard]] std::string config_with_simple_driver() {
        return std::string(R"({"driver":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
               R"(","load":["simple/simple.so"]},"resolver":{"use_custom_server":false},"domains":[]})";
    }

    /// Config with no drivers loaded (driver_dir exists, empty load list).
    [[nodiscard]] std::string config_no_drivers() {
        return std::string(R"({"driver":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
               R"(","load":[]},"resolver":{"use_custom_server":false},"domains":[]})";
    }

    /// Config that loads a driver file that does not exist → BadDriverException.
    [[nodiscard]] std::string config_bad_driver() {
        return std::string(R"({"driver":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
               R"(","load":["definitely_missing_driver.so"]},"resolver":{"use_custom_server":false},"domains":[]})";
    }

    constexpr std::string_view RESOLVER_DEFAULT = R"({"driver":{"auto_discover":false,"load":[]},"resolver":{"use_custom_server":false},"domains":[]})";
    constexpr std::string_view RESOLVER_URI_SERVERS = R"({"driver":{"auto_discover":false,"load":[]},"resolver":{"use_custom_server":true,"servers":[{"address":"https://1.1.1.1/dns-query","port":443}]},"domains":[]})";
    constexpr std::string_view RESOLVER_BARE_SERVERS = R"({"driver":{"auto_discover":false,"load":[]},"resolver":{"use_custom_server":true,"servers":[{"address":"8.8.8.8","port":53}]},"domains":[]})";
    constexpr std::string_view RESOLVER_LEGACY = R"({"driver":{"auto_discover":false,"load":[]},"resolver":{"use_custom_server":true,"address":"9.9.9.9","port":53},"domains":[]})";

    /// Any interface name known to the OS (loopback at minimum).
    [[nodiscard]] std::string any_interface_name() {
        const auto interfaces = InterfaceUtil::get_interfaces();
        if (interfaces.empty()) {
            return {};
        }
        return interfaces.front();
    }
} // namespace

// ===========================================================================
//  parse_and_dispatch — routing
// ===========================================================================

TEST(CliParseTest, NoArgs_ReturnsExitEarlyFailure) {
    auto argv = make_argv({"yaddnsc"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_FALSE(outcome.should_run);
    EXPECT_NE(outcome.exit_code, 0);
}

TEST(CliParseTest, VersionFlag_ExitsZero) {
    auto argv = make_argv({"yaddnsc", "--version"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, 0);
}

TEST(CliParseTest, ShortVersionFlag_ExitsZero) {
    auto argv = make_argv({"yaddnsc", "-v"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, 0);
}

TEST(CliParseTest, HelpFlag_ExitsZero) {
    auto argv = make_argv({"yaddnsc", "--help"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, 0);
}

TEST(CliParseTest, UnknownSubcommand_Fails) {
    auto argv = make_argv({"yaddnsc", "frobnicate"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_NE(outcome.exit_code, 0);
}

TEST(CliParseTest, Run_DefaultConfig) {
    auto argv = make_argv({"yaddnsc", "run"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.should_run);
    EXPECT_FALSE(outcome.exit_early);
    EXPECT_EQ(outcome.config_path, "config.json");
    EXPECT_FALSE(outcome.verbose);
}

TEST(CliParseTest, Run_DebugFlag) {
    auto argv = make_argv({"yaddnsc", "run", "-d"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.should_run);
    EXPECT_TRUE(outcome.verbose);
}

TEST(CliParseTest, Run_WithConfigPath) {
    TempConfigFile cfg{std::string(Fixtures::MINIMAL_CONFIG)};
    auto argv = make_argv({"yaddnsc", "run", "-c", cfg.path()});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.should_run);
    EXPECT_EQ(outcome.config_path, cfg.path());
}

TEST(CliParseTest, Run_NonExistentConfig_Fails) {
    auto argv = make_argv({"yaddnsc", "run", "-c", "/nonexistent/yaddnsc_config.json"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_FALSE(outcome.should_run);
    EXPECT_NE(outcome.exit_code, 0);
}

TEST(CliParseTest, Dns_WithoutSubcommand_Fails) {
    auto argv = make_argv({"yaddnsc", "dns"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_NE(outcome.exit_code, 0);
}

// ===========================================================================
//  config subcommand — executors
// ===========================================================================

TEST(CliConfigTest, ExecuteShow_ValidConfig_ReturnsZero) {
    TempConfigFile cfg{std::string(Fixtures::MINIMAL_CONFIG)};
    EXPECT_EQ(Cli::execute_config_show(cfg.path()), EXIT_SUCCESS);
}

TEST(CliConfigTest, ExecuteShow_InvalidJson_Throws) {
    // execute_config_show does not catch parse errors — they propagate.
    TempConfigFile cfg{std::string(Fixtures::INVALID_JSON)};
    EXPECT_THROW(Cli::execute_config_show(cfg.path()), std::runtime_error);
}

TEST(CliConfigTest, ExecuteShow_MissingFile_Throws) {
    EXPECT_THROW(Cli::execute_config_show("/nonexistent/yaddnsc_config.json"), std::runtime_error);
}

TEST(CliConfigTest, ExecuteTest_ValidConfig_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    EXPECT_EQ(Cli::execute_config_test(cfg.path()), EXIT_SUCCESS);
}

TEST(CliConfigTest, ExecuteTest_Quiet_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    EXPECT_EQ(Cli::execute_config_test(cfg.path(), /*quiet=*/true), EXIT_SUCCESS);
}

TEST(CliConfigTest, ExecuteTest_EmptyDriverDir_ReturnsFailure) {
    // driver_dir set but empty → ConfigVerificationException at load time.
    TempConfigFile cfg(R"({"driver":{"auto_discover":false,"driver_dir":"","load":["simple/simple.so"]},"resolver":{"use_custom_server":false},"domains":[]})");
    EXPECT_EQ(Cli::execute_config_test(cfg.path()), EXIT_FAILURE);
}

TEST(CliConfigTest, ExecuteTest_BadDriver_ReturnsFailure) {
    // A driver file that does not exist → BadDriverException (YaddnscException).
    TempConfigFile cfg(config_bad_driver());
    EXPECT_EQ(Cli::execute_config_test(cfg.path()), EXIT_FAILURE);
}

TEST(CliConfigTest, ExecuteTest_InvalidJson_ReturnsFailure) {
    TempConfigFile cfg{std::string(Fixtures::INVALID_JSON)};
    EXPECT_EQ(Cli::execute_config_test(cfg.path()), EXIT_FAILURE);
}

TEST(CliConfigTest, ExecuteTest_MissingFile_ReturnsFailure) {
    EXPECT_EQ(Cli::execute_config_test("/nonexistent/yaddnsc_config.json"), EXIT_FAILURE);
}

TEST(CliConfigTest, ExecuteTest_InvalidDomain_ReturnsFailure) {
    // A domain whose referenced driver is not loaded → ConfigVerificationException
    // from the validator (after drivers load successfully).
    const std::string invalid_domain_config = std::string("{\"driver\":{\"auto_discover\":false,\"driver_dir\":\"") +
                                              TEST_DRIVER_DIR +
                                              "\",\"load\":[\"simple/simple.so\"]},\"resolver\":{\"use_custom_server\":false},\"domains\":[{\"name\":\"example.com\",\"driver\":\"cloudflare\",\"subdomains\":[{\"name\":\"www\",\"type\":\"a\",\"ip_source\":\"http\",\"ip_source_param\":\"https://api.ipify.org\"}]}]}";
    TempConfigFile cfg(invalid_domain_config);
    EXPECT_EQ(Cli::execute_config_test(cfg.path()), EXIT_FAILURE);
}

// ===========================================================================
//  config subcommand — end-to-end via parse_and_dispatch
// ===========================================================================

TEST(CliConfigTest, Parse_ShowSubcommand_ReturnsZero) {
    TempConfigFile cfg{std::string(Fixtures::MINIMAL_CONFIG)};
    auto argv = make_argv({"yaddnsc", "config", "show", "-c", cfg.path()});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliConfigTest, Parse_TestSubcommand_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    auto argv = make_argv({"yaddnsc", "config", "test", "-c", cfg.path()});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliConfigTest, Parse_TestQuietAlias_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    auto argv = make_argv({"yaddnsc", "config", "t", "-q", "-c", cfg.path()});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliConfigTest, Parse_TestInvalidJson_ReturnsFailure) {
    TempConfigFile cfg{std::string(Fixtures::INVALID_JSON)};
    auto argv = make_argv({"yaddnsc", "config", "test", "-c", cfg.path()});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_NE(outcome.exit_code, EXIT_SUCCESS);
}

// ===========================================================================
//  driver subcommand — executors
// ===========================================================================

TEST(CliDriverTest, ExecuteList_WithDrivers_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    EXPECT_EQ(Cli::execute_driver_list(cfg.path()), EXIT_SUCCESS);
}

TEST(CliDriverTest, ExecuteList_NoDrivers_ReturnsZero) {
    TempConfigFile cfg(config_no_drivers());
    EXPECT_EQ(Cli::execute_driver_list(cfg.path()), EXIT_SUCCESS);
}

TEST(CliDriverTest, ExecuteInfo_KnownDriver_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    EXPECT_EQ(Cli::execute_driver_info(cfg.path(), "simple"), EXIT_SUCCESS);
}

TEST(CliDriverTest, ExecuteInfo_UnknownDriver_ReturnsFailure) {
    TempConfigFile cfg(config_with_simple_driver());
    EXPECT_EQ(Cli::execute_driver_info(cfg.path(), "not_a_driver"), EXIT_FAILURE);
}

// ===========================================================================
//  driver subcommand — end-to-end via parse_and_dispatch
// ===========================================================================

TEST(CliDriverTest, Parse_ListSubcommand_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    auto argv = make_argv({"yaddnsc", "driver", "list", "-c", cfg.path()});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliDriverTest, Parse_InfoSubcommand_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    auto argv = make_argv({"yaddnsc", "driver", "info", "-c", cfg.path(), "simple"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliDriverTest, Parse_InfoUnknownDriver_Fails) {
    TempConfigFile cfg(config_with_simple_driver());
    auto argv = make_argv({"yaddnsc", "driver", "info", "-c", cfg.path(), "not_a_driver"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_NE(outcome.exit_code, EXIT_SUCCESS);
}

// ===========================================================================
//  interface subcommand
// ===========================================================================

TEST(CliInterfaceTest, ExecuteList_ReturnsZero) {
    EXPECT_EQ(Cli::execute_interface_list(), EXIT_SUCCESS);
}

TEST(CliInterfaceTest, ExecuteIp_KnownInterface_ReturnsZero) {
    const auto iface = any_interface_name();
    ASSERT_FALSE(iface.empty()) << "no network interfaces available";
    EXPECT_EQ(Cli::execute_interface_ip(iface), EXIT_SUCCESS);
}

TEST(CliInterfaceTest, ExecuteIp_UnknownInterface_ReturnsFailure) {
    EXPECT_EQ(Cli::execute_interface_ip("yaddnsc_definitely_no_such_iface"), EXIT_FAILURE);
}

TEST(CliInterfaceTest, Parse_ListSubcommand_ReturnsZero) {
    auto argv = make_argv({"yaddnsc", "interface", "list"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliInterfaceTest, Parse_IpSubcommand_ReturnsZero) {
    const auto iface = any_interface_name();
    ASSERT_FALSE(iface.empty()) << "no network interfaces available";
    auto argv = make_argv({"yaddnsc", "interface", "ip", iface});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliInterfaceTest, Parse_IpUnknownInterface_Fails) {
    auto argv = make_argv({"yaddnsc", "interface", "ip", "yaddnsc_definitely_no_such_iface"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_NE(outcome.exit_code, EXIT_SUCCESS);
}

// ===========================================================================
//  dns subcommand — executors
// ===========================================================================

TEST(CliDnsTest, ExecuteResolver_Default_ReturnsZero) {
    TempConfigFile cfg{std::string(RESOLVER_DEFAULT)};
    EXPECT_EQ(Cli::execute_dns_resolver(cfg.path()), EXIT_SUCCESS);
}

TEST(CliDnsTest, ExecuteResolver_UriServers_ReturnsZero) {
    TempConfigFile cfg{std::string(RESOLVER_URI_SERVERS)};
    EXPECT_EQ(Cli::execute_dns_resolver(cfg.path()), EXIT_SUCCESS);
}

TEST(CliDnsTest, ExecuteResolver_BareServers_ReturnsZero) {
    TempConfigFile cfg{std::string(RESOLVER_BARE_SERVERS)};
    EXPECT_EQ(Cli::execute_dns_resolver(cfg.path()), EXIT_SUCCESS);
}

TEST(CliDnsTest, ExecuteResolver_LegacyServer_ReturnsZero) {
    TempConfigFile cfg{std::string(RESOLVER_LEGACY)};
    EXPECT_EQ(Cli::execute_dns_resolver(cfg.path()), EXIT_SUCCESS);
}

TEST(CliDnsTest, ExecuteResolve_UnknownType_ReturnsFailure) {
    TempConfigFile cfg{std::string(RESOLVER_DEFAULT)};
    EXPECT_EQ(Cli::execute_dns_resolve(cfg.path(), "example.com", "BOGUS"), EXIT_FAILURE);
}

TEST(CliDnsTest, ExecuteResolve_Success_ReturnsZero) {
    g_stub_behavior = [](RecordKind) -> StubResult {
        return std::vector<std::uint8_t>(std::begin(Fixtures::DnsWire::SIMPLE_A_RESPONSE),
                                         std::end(Fixtures::DnsWire::SIMPLE_A_RESPONSE));
    };
    TempConfigFile cfg{std::string(RESOLVER_DEFAULT)};
    EXPECT_EQ(Cli::execute_dns_resolve(cfg.path(), "example.com", "A"), EXIT_SUCCESS);
}

TEST(CliDnsTest, ExecuteResolve_NoRecords_ReturnsZero) {
    g_stub_behavior = [](RecordKind) -> StubResult { return empty_response(); };
    TempConfigFile cfg{std::string(RESOLVER_DEFAULT)};
    EXPECT_EQ(Cli::execute_dns_resolve(cfg.path(), "example.com", "A"), EXIT_SUCCESS);
}

TEST(CliDnsTest, ExecuteResolve_Error_ReturnsZero) {
    g_stub_behavior = [](RecordKind) -> StubResult {
        return std::unexpected(DnsErrorInfo{DnsError::NX_DOMAIN, "Domain example.com does not exist (NXDOMAIN)"});
    };
    TempConfigFile cfg{std::string(RESOLVER_DEFAULT)};
    EXPECT_EQ(Cli::execute_dns_resolve(cfg.path(), "example.com", "A"), EXIT_SUCCESS);
}

// ===========================================================================
//  dns subcommand — end-to-end via parse_and_dispatch
// ===========================================================================

TEST(CliDnsTest, Parse_ResolveSubcommand_ReturnsZero) {
    g_stub_behavior = [](RecordKind) -> StubResult {
        return std::vector<std::uint8_t>(std::begin(Fixtures::DnsWire::SIMPLE_A_RESPONSE),
                                         std::end(Fixtures::DnsWire::SIMPLE_A_RESPONSE));
    };
    TempConfigFile cfg{std::string(RESOLVER_DEFAULT)};
    auto argv = make_argv({"yaddnsc", "dns", "resolve", "-c", cfg.path(), "example.com"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliDnsTest, Parse_ResolveAlias_ReturnsZero) {
    g_stub_behavior = [](RecordKind) -> StubResult {
        return std::vector<std::uint8_t>(std::begin(Fixtures::DnsWire::SIMPLE_A_RESPONSE),
                                         std::end(Fixtures::DnsWire::SIMPLE_A_RESPONSE));
    };
    TempConfigFile cfg{std::string(RESOLVER_DEFAULT)};
    auto argv = make_argv({"yaddnsc", "dns", "r", "-c", cfg.path(), "example.com", "--type", "AAAA"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliDnsTest, Parse_ResolveInvalidType_Fails) {
    TempConfigFile cfg{std::string(RESOLVER_DEFAULT)};
    auto argv = make_argv({"yaddnsc", "dns", "resolve", "-c", cfg.path(), "example.com", "--type", "BOGUS"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_NE(outcome.exit_code, EXIT_SUCCESS);
}

TEST(CliDnsTest, Parse_ResolverSubcommand_ReturnsZero) {
    TempConfigFile cfg{std::string(RESOLVER_URI_SERVERS)};
    auto argv = make_argv({"yaddnsc", "dns", "resolver", "-c", cfg.path()});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}

// ===========================================================================
//  info subcommand — end-to-end via parse_and_dispatch
// ===========================================================================

TEST(CliInfoTest, Parse_InfoSubcommand_ReturnsZero) {
    auto argv = make_argv({"yaddnsc", "info"});
    const auto outcome = Cli::parse_and_dispatch(argv.argc(), argv.data());

    EXPECT_TRUE(outcome.exit_early);
    EXPECT_EQ(outcome.exit_code, EXIT_SUCCESS);
}
