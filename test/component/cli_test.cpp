//
// Component tests for the CLI layer and the composition root.
//
// Covers:
//   - Cli::parse: subcommand routing, aliases, options, --help/--version,
//     parse errors — pure parsing, no business side effects.
//   - Composition::dispatch: end-to-end golden behaviour for the diagnostic
//     commands (config show/test, driver list/info, interface list/ip,
//     dns resolver, info), incl. exit codes and stderr wording.
//   - Cli presenters: config show redaction, dns resolve outcomes,
//     config test prefixes, the "Error: ..." catch-all.
//   - Diagnostics handlers over fake ports (no real DNS / drivers).
//   - Shell completion scripts stay in sync with the parser's
//     command/alias set.
//
// Requires a built driver .so (simple) at TEST_DRIVER_DIR for the
// config-test and driver-command dispatch paths.
// =============================================================================

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <expected>
#include <fcntl.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "application/diagnostics.h"
#include "application/ports/driver_catalog.h"
#include "cli/command.h"
#include "cli/parser.h"
#include "cli/presenter.h"
#include "composition/bootstrap.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "fixtures/sample_config.h"
#include "infrastructure/network/system_network_interfaces.h"
#include "mocks/mock_ports.h"

// ===========================================================================
//  Helpers — argv construction + temp config files + output capture
// ===========================================================================

namespace {

struct Argv {
    std::vector<std::string> storage;
    std::vector<char*> ptrs;

    [[nodiscard]] int argc() const { return static_cast<int>(ptrs.size()); }

    [[nodiscard]] char** data() { return ptrs.data(); }
};

/// Build argv that stays alive for the duration of the call: the returned
/// struct owns both the string storage and the char* pointers.
[[nodiscard]] Argv make_argv(std::vector<std::string> args) {
    Argv argv;
    argv.storage = std::move(args);
    argv.ptrs.reserve(argv.storage.size());
    for (auto& arg : argv.storage) {
        argv.ptrs.push_back(arg.data());
    }
    return argv;
}

/// Parse helper: builds argv and runs the pure parser.
[[nodiscard]] Cli::ParseResult parse(std::vector<std::string> args) {
    auto argv = make_argv(std::move(args));
    return Cli::parse(argv.argc(), argv.data());
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

    TempConfigFile(const TempConfigFile&) = delete;
    TempConfigFile& operator=(const TempConfigFile&) = delete;

    [[nodiscard]] const std::string& path() const { return path_; }

private:
    [[nodiscard]] static std::string make_unique_path() {
        static std::atomic<unsigned> counter{0};
        const auto path =
            std::filesystem::temp_directory_path() /
            ("yaddnsc_cli_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)) + ".json");
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

/// Config that loads a driver file that does not exist → PluginLoadException.
[[nodiscard]] std::string config_bad_driver() {
    return std::string(R"({"driver":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
           R"(","load":["definitely_missing_driver.so"]},"resolver":{"use_custom_server":false},"domains":[]})";
}

/// Redirect a stream (STDOUT_FILENO or STDERR_FILENO) to a temp file so
/// output can be asserted. Restores on destruction (or when str() runs).
class StreamCapture {
public:
    explicit StreamCapture(int fd = STDOUT_FILENO) : target_fd_(fd), path_(make_unique_path()) {
        flush_out();
        saved_fd_ = ::dup(target_fd_);
        file_fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        ::dup2(file_fd_, target_fd_);
    }

    ~StreamCapture() {
        restore();
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    StreamCapture(const StreamCapture&) = delete;
    StreamCapture& operator=(const StreamCapture&) = delete;

    /// Restore the stream and return everything written so far.
    [[nodiscard]] std::string str() {
        restore();
        std::ifstream in(path_);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

private:
    // std::println writes to the stdio buffer while CLI11 and gtest use
    // std::cout — both buffers must be drained around a redirect.
    static void flush_out() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
    }

    void restore() {
        if (saved_fd_ == -1) {
            return;
        }
        flush_out();
        ::dup2(saved_fd_, target_fd_);
        ::close(saved_fd_);
        ::close(file_fd_);
        saved_fd_ = -1;
    }

    [[nodiscard]] static std::filesystem::path make_unique_path() {
        static std::atomic<unsigned> counter{0};
        return std::filesystem::temp_directory_path() /
               ("yaddnsc_cli_out_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)) + ".txt");
    }

    int target_fd_;
    std::filesystem::path path_;
    int saved_fd_{-1};
    int file_fd_{-1};
};

using StdoutCapture = StreamCapture;

/// Any interface name known to the OS (loopback at minimum).
[[nodiscard]] std::string any_interface_name() {
    const SystemNetworkInterfaces interfaces;
    const auto names = interfaces.names();
    return names.empty() ? std::string{} : names.front();
}
}  // namespace

// ===========================================================================
//  Cli::parse — routing, aliases, options (pure parsing)
// ===========================================================================

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

// ===========================================================================
//  Composition::dispatch — config show / config test
// ===========================================================================

TEST(CliConfigTest, DispatchShow_ValidConfig_ReturnsZero) {
    TempConfigFile cfg{std::string(Fixtures::MINIMAL_CONFIG)};
    EXPECT_EQ(Composition::dispatch(Cli::ConfigShowCommand{cfg.path()}), EXIT_SUCCESS);
}

TEST(CliConfigTest, DispatchShow_InvalidJson_ReturnsFailure) {
    TempConfigFile cfg{std::string(Fixtures::INVALID_JSON)};
    EXPECT_EQ(Composition::dispatch(Cli::ConfigShowCommand{cfg.path()}), EXIT_FAILURE);
}

TEST(CliConfigTest, DispatchShow_InvalidJson_PrintsErrorToStderr) {
    TempConfigFile cfg{std::string(Fixtures::INVALID_JSON)};

    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Composition::dispatch(Cli::ConfigShowCommand{cfg.path()}), EXIT_FAILURE);
    EXPECT_NE(err.str().find("Error: "), std::string::npos);
}

TEST(CliConfigTest, DispatchShow_MissingFile_ReturnsFailure) {
    EXPECT_EQ(Composition::dispatch(Cli::ConfigShowCommand{"/nonexistent/yaddnsc_config.json"}), EXIT_FAILURE);
}

// Intentional behaviour: config show redacts sensitive driver_param fields by
// default. Rule: an object member is sensitive when its lower-cased key
// contains "token", "password", "secret" or "key"; the whole value is
// replaced with "***". Only fake placeholder values are used here — real
// tokens must never appear in golden files.
TEST(CliConfigTest, DispatchShow_RedactsSensitiveDriverParams) {
    const std::string config_json = R"({
        "driver": {"auto_discover": false, "load": []},
        "resolver": {"use_custom_server": false},
        "domains": [{
            "name": "example.com",
            "update_interval": 300,
            "driver": "simple",
            "subdomains": [{
                "name": "www",
                "type": "a",
                "ip_source": "http",
                "ip_source_param": "https://api.ipify.org",
                "driver_param": {
                    "url": "https://example.com/update?token={token}",
                    "api_token": "fake-token-0001",
                    "Password": "fake-password-0002",
                    "ttl": 600,
                    "nested": {"secret_key": "fake-secret-0003", "record_id": "R123"},
                    "list": [{"accessKey": "fake-key-0004"}, "plain-text"],
                    "note": "not-a-secret"
                }
            }]
        }]
    })";
    TempConfigFile cfg(config_json);

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::ConfigShowCommand{cfg.path()}), EXIT_SUCCESS);
    const std::string out = capture.str();

    // Every sensitive value is replaced, whatever its original type or nesting.
    EXPECT_NE(out.find(R"("api_token":"***")"), std::string::npos);
    EXPECT_NE(out.find(R"("Password":"***")"), std::string::npos);
    EXPECT_NE(out.find(R"("secret_key":"***")"), std::string::npos);
    EXPECT_NE(out.find(R"("accessKey":"***")"), std::string::npos);

    // No fake secret leaks anywhere in the output.
    EXPECT_EQ(out.find("fake-"), std::string::npos);

    // Non-sensitive fields and values survive untouched, including a template
    // placeholder that merely mentions "token" in a *value* position.
    EXPECT_NE(out.find(R"("note":"not-a-secret")"), std::string::npos);
    EXPECT_NE(out.find(R"("record_id":"R123")"), std::string::npos);
    EXPECT_NE(out.find(R"("ttl":600)"), std::string::npos);
    EXPECT_NE(out.find("https://example.com/update?token={token}"), std::string::npos);
    EXPECT_NE(out.find(R"("plain-text")"), std::string::npos);
}

TEST(CliConfigTest, DispatchTest_ValidConfig_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_SUCCESS);
}

TEST(CliConfigTest, DispatchTest_ValidConfig_PrintsPassed) {
    TempConfigFile cfg(config_with_simple_driver());

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("Configuration file test passed"), std::string::npos);
}

TEST(CliConfigTest, DispatchTest_Quiet_PrintsNothing) {
    TempConfigFile cfg(config_with_simple_driver());

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path(), /*quiet=*/true}), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "");
}

TEST(CliConfigTest, DispatchTest_EmptyDriverDir_ReturnsFailure) {
    // driver_dir set but empty → ConfigVerificationException at load time.
    TempConfigFile cfg(
        R"({"driver":{"auto_discover":false,"driver_dir":"","load":["simple/simple.so"]},"resolver":{"use_custom_server":false},"domains":[]})");
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_FAILURE);
}

TEST(CliConfigTest, DispatchTest_BadDriver_ReturnsFailure) {
    // A driver file that does not exist → PluginLoadException (YaddnscException).
    TempConfigFile cfg(config_bad_driver());
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_FAILURE);
}

TEST(CliConfigTest, DispatchTest_InvalidJson_ReturnsFailure) {
    TempConfigFile cfg{std::string(Fixtures::INVALID_JSON)};
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_FAILURE);
}

TEST(CliConfigTest, DispatchTest_MissingFile_ReturnsFailure) {
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{"/nonexistent/yaddnsc_config.json"}), EXIT_FAILURE);
}

TEST(CliConfigTest, DispatchTest_DriverNotLoaded_ReturnsFailure) {
    // A domain whose referenced driver is not loaded → environment validation
    // failure (after drivers load successfully). update_interval is set so
    // static validation passes and the environment check is what fails.
    const std::string invalid_domain_config =
        std::string("{\"driver\":{\"auto_discover\":false,\"driver_dir\":\"") + TEST_DRIVER_DIR +
        "\",\"load\":[\"simple/"
        "simple.so\"]},\"resolver\":{\"use_custom_server\":false},\"domains\":[{\"name\":\"example.com\",\"update_"
        "interval\":300,\"driver\":\"cloudflare\",\"subdomains\":[{\"name\":\"www\",\"type\":\"a\",\"ip_source\":"
        "\"http\",\"ip_source_param\":\"https://api.ipify.org\"}]}]}";
    TempConfigFile cfg(invalid_domain_config);

    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_FAILURE);
    EXPECT_NE(err.str().find("Configuration verification failed: Driver cloudflare not found"), std::string::npos);
}

// parse → dispatch round trip: the wiring from argv to exit code.
TEST(CliConfigTest, ParseThenDispatch_TestQuietAlias_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    auto parsed = parse({"yaddnsc", "config", "t", "-q", "-c", cfg.path()});
    ASSERT_TRUE(parsed.command.has_value());

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(*parsed.command), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "");
}

// ===========================================================================
//  Composition::dispatch — driver list / info
// ===========================================================================

TEST(CliDriverTest, DispatchList_WithDrivers_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DriverListCommand{cfg.path()}), EXIT_SUCCESS);
    const std::string out = capture.str();
    EXPECT_NE(out.find("Loaded drivers (1):"), std::string::npos);
    EXPECT_NE(out.find("simple"), std::string::npos);
}

TEST(CliDriverTest, DispatchList_NoDrivers_ReturnsZero) {
    TempConfigFile cfg(config_no_drivers());

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DriverListCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("No drivers loaded."), std::string::npos);
}

TEST(CliDriverTest, DispatchInfo_KnownDriver_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DriverInfoCommand{cfg.path(), "simple"}), EXIT_SUCCESS);
    const std::string out = capture.str();
    EXPECT_NE(out.find("Name:        simple"), std::string::npos);
    EXPECT_NE(out.find("Version:     "), std::string::npos);
}

TEST(CliDriverTest, DispatchInfo_UnknownDriver_ReturnsFailure) {
    TempConfigFile cfg(config_with_simple_driver());

    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Composition::dispatch(Cli::DriverInfoCommand{cfg.path(), "not_a_driver"}), EXIT_FAILURE);
    EXPECT_NE(err.str().find("Error: Driver 'not_a_driver' is not loaded"), std::string::npos);
}

// ===========================================================================
//  Composition::dispatch — interface list / ip
// ===========================================================================

TEST(CliInterfaceTest, DispatchList_ReturnsZero) {
    EXPECT_EQ(Composition::dispatch(Cli::InterfaceListCommand{}), EXIT_SUCCESS);
}

TEST(CliInterfaceTest, DispatchIp_KnownInterface_ReturnsZero) {
    const auto iface = any_interface_name();
    ASSERT_FALSE(iface.empty()) << "no network interfaces available";

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::InterfaceIpCommand{iface}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("Interface: " + iface), std::string::npos);
}

TEST(CliInterfaceTest, DispatchIp_UnknownInterface_ReturnsFailure) {
    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Composition::dispatch(Cli::InterfaceIpCommand{"yaddnsc_definitely_no_such_iface"}), EXIT_FAILURE);
    EXPECT_NE(err.str().find("Error: Interface yaddnsc_definitely_no_such_iface not found"), std::string::npos);
}

// ===========================================================================
//  Composition::dispatch — dns resolver / info
// ===========================================================================

TEST(CliDnsTest, DispatchResolver_Default_ReturnsZero) {
    TempConfigFile cfg{std::string(Fixtures::MINIMAL_CONFIG)};

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolverCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("DNS resolver configuration:"), std::string::npos);
}

TEST(CliDnsTest, DispatchResolver_UriServers_ReturnsZero) {
    TempConfigFile cfg{
        R"({"driver":{"auto_discover":false,"load":[]},"resolver":{"use_custom_server":true,"servers":[{"address":"https://1.1.1.1/dns-query","port":443}]},"domains":[]})"};

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolverCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("https://1.1.1.1/dns-query"), std::string::npos);
}

TEST(CliDnsTest, DispatchResolver_BareServers_ReturnsZero) {
    TempConfigFile cfg{
        R"({"driver":{"auto_discover":false,"load":[]},"resolver":{"use_custom_server":true,"servers":[{"address":"8.8.8.8","port":53}]},"domains":[]})"};

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolverCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("8.8.8.8:53"), std::string::npos);
}

TEST(CliDnsTest, DispatchResolver_LegacyServer_ReturnsZero) {
    TempConfigFile cfg{
        R"({"driver":{"auto_discover":false,"load":[]},"resolver":{"use_custom_server":true,"address":"9.9.9.9","port":53},"domains":[]})"};

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolverCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("Server: 9.9.9.9:53"), std::string::npos);
}

// The dns resolve dispatch path builds a real resolver dispatcher from the
// config, but an unknown record type short-circuits before any socket I/O:
// Diagnostics::dns_resolve returns "no lookup" and the presenter reports the
// valid set. Command is constructed directly (the parser would reject the
// type), keeping the test on loopback-free, deterministic ground.
TEST(CliDnsTest, DispatchResolve_UnknownType_PrintsValidTypes) {
    TempConfigFile cfg{std::string(Fixtures::MINIMAL_CONFIG)};

    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolveCommand{cfg.path(), "example.com", "BOGUS"}), EXIT_FAILURE);
    EXPECT_EQ(err.str(), "Error: unknown record type 'BOGUS'.\nValid types: A, AAAA, TXT\n");
}

TEST(CliDnsTest, DispatchResolve_MissingConfig_ReturnsFailure) {
    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolveCommand{"/nonexistent/yaddnsc_config.json", "example.com", "A"}),
              EXIT_FAILURE);
    EXPECT_NE(err.str().find("Error: "), std::string::npos);
}

TEST(CliInfoTest, DispatchInfo_PrintsKeyFields) {
    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::InfoCommand{}), EXIT_SUCCESS);
    const std::string out = capture.str();

    EXPECT_NE(out.find("Version:"), std::string::npos);
    EXPECT_NE(out.find("Build ID:"), std::string::npos);
    EXPECT_NE(out.find("DNS resolver:"), std::string::npos);
    EXPECT_NE(out.find("Min update interval:"), std::string::npos);
}

// Locked CLI behaviour: -v/--version prints "yaddnsc/<version>" and exits zero.
TEST(CliInfoTest, VersionFlag_PrintsProgramAndVersion) {
    StdoutCapture capture;
    const auto result = parse({"yaddnsc", "--version"});
    const std::string out = capture.str();

    EXPECT_EQ(result.exit_code, EXIT_SUCCESS);
    EXPECT_NE(out.find("yaddnsc/"), std::string::npos);
}

// ===========================================================================
//  Diagnostics handlers — fake ports, no real DNS / drivers / interfaces
// ===========================================================================

TEST(CliDiagnosticsTest, DnsResolve_UnknownType_ReturnsNoLookup) {
    MockDnsResolverPort resolver;
    const auto outcome = Diagnostics::dns_resolve(resolver, "example.com", "BOGUS");

    EXPECT_EQ(outcome.host, "example.com");
    EXPECT_EQ(outcome.type_text, "BOGUS");
    EXPECT_FALSE(outcome.lookup.has_value());
}

TEST(CliDiagnosticsTest, DnsResolve_TypeIsCaseInsensitive) {
    MockDnsResolverPort resolver;
    EXPECT_CALL(resolver, resolve("example.com", RecordKind::AAAA))
        .WillOnce(::testing::Return(std::vector<std::string>{"::1"}));

    const auto outcome = Diagnostics::dns_resolve(resolver, "example.com", "aaaa");
    ASSERT_TRUE(outcome.lookup.has_value());
    ASSERT_TRUE(outcome.lookup->has_value());
    EXPECT_EQ((*outcome.lookup)->front(), "::1");
}

TEST(CliDiagnosticsTest, DnsResolve_ErrorPassesThrough) {
    MockDnsResolverPort resolver;
    EXPECT_CALL(resolver, resolve("example.com", RecordKind::A))
        .WillOnce(::testing::Return(std::unexpected(DnsErrorInfo{DnsError::NX_DOMAIN, "nxdomain"})));

    const auto outcome = Diagnostics::dns_resolve(resolver, "example.com", "A");
    ASSERT_TRUE(outcome.lookup.has_value());
    ASSERT_FALSE(outcome.lookup->has_value());
    EXPECT_EQ(outcome.lookup->error().message, "nxdomain");
}

TEST(CliDiagnosticsTest, ListDrivers_EmptyCatalog) {
    MockDriverCatalogPort catalog;
    ON_CALL(catalog, loaded_drivers()).WillByDefault(::testing::Return(std::vector<std::string>{}));

    EXPECT_TRUE(Diagnostics::list_drivers(catalog).empty());
}

TEST(CliDiagnosticsTest, ListDrivers_CapturesPerDriverFailure) {
    MockDriverCatalogPort catalog;
    ON_CALL(catalog, loaded_drivers()).WillByDefault(::testing::Return(std::vector<std::string>{"good", "bad"}));
    ON_CALL(catalog, describe("good"))
        .WillByDefault(
            ::testing::Return(DriverDescription{.name = "good", .version = "1.0", .author = "a", .description = "d"}));
    ON_CALL(catalog, describe("bad")).WillByDefault(::testing::Throw(std::runtime_error("descriptor exploded")));

    const auto items = Diagnostics::list_drivers(catalog);
    ASSERT_EQ(items.size(), 2);
    EXPECT_TRUE(items[0].detail.has_value());
    EXPECT_EQ(items[0].detail->name, "good");
    EXPECT_FALSE(items[1].detail.has_value());
    EXPECT_EQ(items[1].error, "descriptor exploded");
}

TEST(CliDiagnosticsTest, ListInterfaces_CollectsAddresses) {
    MockNetworkInterfaces interfaces;
    ON_CALL(interfaces, names()).WillByDefault(::testing::Return(std::vector<std::string>{"lo", "eth0"}));
    ON_CALL(interfaces, addresses("lo"))
        .WillByDefault(::testing::Return(std::vector<InetAddress>{InetAddress(*Inet4Address::parse("127.0.0.1"))}));
    ON_CALL(interfaces, addresses("eth0")).WillByDefault(::testing::Return(std::vector<InetAddress>{}));

    const auto items = Diagnostics::list_interfaces(interfaces);
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0].name, "lo");
    EXPECT_EQ(items[0].addresses.size(), 1);
    EXPECT_EQ(items[1].name, "eth0");
    EXPECT_TRUE(items[1].addresses.empty());
}

// ===========================================================================
//  Presenters — stdout/stderr text and exit codes
// ===========================================================================

TEST(CliPresenterTest, DnsResolve_UnknownType_PrintsValidTypes) {
    Diagnostics::DnsResolveOutcome outcome{.host = "example.com", .type_text = "BOGUS", .lookup = std::nullopt};

    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Cli::present_dns_resolve(outcome), EXIT_FAILURE);
    EXPECT_EQ(err.str(), "Error: unknown record type 'BOGUS'.\nValid types: A, AAAA, TXT\n");
}

TEST(CliPresenterTest, DnsResolve_Failure_PrintsMessageAndSucceeds) {
    Diagnostics::DnsResolveOutcome outcome{
        .host = "example.com",
        .type_text = "A",
        .lookup = std::unexpected(DnsErrorInfo{DnsError::NX_DOMAIN, "Domain example.com does not exist (NXDOMAIN)"})};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolve(outcome), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "DNS lookup for example.com (A) failed: Domain example.com does not exist (NXDOMAIN)\n");
}

TEST(CliPresenterTest, DnsResolve_NoRecords_PrintsMessageAndSucceeds) {
    Diagnostics::DnsResolveOutcome outcome{
        .host = "example.com", .type_text = "AAAA", .lookup = std::vector<std::string>{}};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolve(outcome), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "DNS lookup for example.com (AAAA) returned no records\n");
}

TEST(CliPresenterTest, DnsResolve_Records_PrintsResultBlock) {
    Diagnostics::DnsResolveOutcome outcome{
        .host = "example.com", .type_text = "A", .lookup = std::vector<std::string>{"192.0.2.1", "192.0.2.2"}};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolve(outcome), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "DNS lookup result:\n  Host:  example.com\n  Type:  A\n  Value: 192.0.2.1, 192.0.2.2\n");
}

TEST(CliPresenterTest, ConfigTest_Success_PrintsPassed) {
    StdoutCapture capture;
    EXPECT_EQ(Cli::present_config_test({.quiet = false, .error = std::nullopt}), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "Configuration file test passed\n");
}

TEST(CliPresenterTest, ConfigTest_QuietSuccess_PrintsNothing) {
    StdoutCapture capture;
    EXPECT_EQ(Cli::present_config_test({.quiet = true, .error = std::nullopt}), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "");
}

TEST(CliPresenterTest, ConfigTest_ErrorPrefixes) {
    using Error = Diagnostics::ConfigTestError;

    {
        StreamCapture err{STDERR_FILENO};
        EXPECT_EQ(Cli::present_config_test(
                      {.quiet = false, .error = Error{.kind = Error::Kind::VERIFICATION, .message = "m1"}}),
                  EXIT_FAILURE);
        EXPECT_EQ(err.str(), "Configuration verification failed: m1\n");
    }
    {
        StreamCapture err{STDERR_FILENO};
        EXPECT_EQ(
            Cli::present_config_test({.quiet = false, .error = Error{.kind = Error::Kind::FATAL, .message = "m2"}}),
            EXIT_FAILURE);
        EXPECT_EQ(err.str(), "Fatal error: unrecoverable exception: m2\n");
    }
    {
        StreamCapture err{STDERR_FILENO};
        EXPECT_EQ(
            Cli::present_config_test({.quiet = false, .error = Error{.kind = Error::Kind::GENERIC, .message = "m3"}}),
            EXIT_FAILURE);
        EXPECT_EQ(err.str(), "Failed to validate configuration: m3\n");
    }
}

TEST(CliPresenterTest, ErrorCatchAll_PrintsToStderr) {
    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Cli::present_error(std::runtime_error("boom")), EXIT_FAILURE);
    EXPECT_EQ(err.str(), "Error: boom\n");
}

// ===========================================================================
//  Shell completion scripts — command/alias set stays in sync with the parser
//
//  Each script is checked for format-specific patterns (a bare substring like
//  "r" would match anything): zsh alias cases "resolve|r)", the bash word
//  walk list, fish's "__fish_seen_subcommand_from ... r" pairs.
// ===========================================================================

TEST(CliCompletionTest, ScriptsCoverEveryCommandAndAlias) {
    const std::filesystem::path template_dir = YADDNSC_TEMPLATE_DIR;

    const std::vector<std::pair<std::string, std::vector<std::string>>> expectations = {
        {"zsh/_yaddnsc",
         {"'run:Run the DDNS client'", "'driver:Manage DDNS driver modules'", "'interface:Query network interfaces'",
          "'dns:DNS lookup and diagnostics'", "'config:Configuration management'", "'info:Show build configuration'",
          "interface|if|net)", "resolve|r)", "show|s)", "test|t)", "--type", "--debug", "--quiet", "--config",
          "--version"}},
        {"bash/yaddnsc",
         {"run driver interface dns config info", "interface|if|net)", "resolve|resolver|show|test|r|s|t", "--config",
          "--debug", "--quiet", "--type", "-v"}},
        {"fish/yaddnsc.fish",
         {"__fish_seen_subcommand_from run driver interface if net dns config info", "resolve r", "show s", "test t",
          "-l config", "-l debug", "-l quiet", "-l type", "-l version"}},
    };

    for (const auto& [script, patterns] : expectations) {
        std::ifstream in(template_dir / script);
        ASSERT_TRUE(in.good()) << "missing completion script: " << script;
        const std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        for (const auto& pattern : patterns) {
            EXPECT_TRUE(content.find(pattern) != std::string::npos)
                << script << " is out of sync with the parser (missing: " << pattern << ")";
        }
    }
}
