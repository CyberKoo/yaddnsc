//
// Component tests for Composition::dispatch — end-to-end golden behaviour of
// the diagnostic commands (config show/test, run validation ordering, driver
// list/info, interface list/ip, dns resolver, info), incl. exit codes and
// stderr wording.
//
// The pure halves live with the unit tests: Cli::parse in
// test/unit/cli/parser_test.cpp, the presenters in test/unit/cli/presenter_test.cpp,
// the completion-script sync check in test/unit/cli/completion_test.cpp, and
// the diagnostics handlers over fake ports in
// test/unit/application/diagnostics_test.cpp.
//
// Requires a built driver .so (simple) at TEST_DRIVER_DIR for the
// config-test and driver-command dispatch paths.
// =============================================================================

#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include "cli/command.h"
#include "composition/bootstrap.h"
#include "fixtures/cli_test_support.h"
#include "fixtures/sample_config.h"
#include "infrastructure/ip_source/system_network_interfaces.h"

using namespace CliTestSupport;

namespace {

/// One statically-valid domain using the HTTP IP source (no interface
/// dependency, so environment validation passes anywhere).
[[nodiscard]] std::string one_http_domain() {
    return R"("domains":[{"name":"example.com","update_interval":300,"driver":"simple",)"
           R"("subdomains":[{"name":"www","type":"a","ip_source":"http",)"
           R"("ip_source_param":"https://api.ipify.org",)"
           R"("driver_params":{"url":"https://example.com/update"}}]}])";
}

/// Config that loads the real "simple" driver from the build tree.
[[nodiscard]] std::string config_with_simple_driver() {
    return std::string(R"({"drivers":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
           R"(","load":["simple/simple.so"]},"resolver":{"use_custom_servers":false},)" + one_http_domain() + "}";
}

/// Config with no drivers loaded (driver_dir exists, empty load list).
[[nodiscard]] std::string config_no_drivers() {
    return std::string(R"({"drivers":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
           R"(","load":[]},"resolver":{"use_custom_servers":false},)" + one_http_domain() + "}";
}

/// Config that loads a driver file that does not exist → PluginLoadException.
[[nodiscard]] std::string config_bad_driver() {
    return std::string(R"({"drivers":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
           R"(","load":["definitely_missing_driver.so"]},"resolver":{"use_custom_servers":false},)" +
           one_http_domain() + "}";
}

/// Config that auto-discovers the real driver directory: an interface-sourced
/// domain with the given driver_params JSON object.
[[nodiscard]] std::string config_with_interface_source(const std::string& iface, const std::string& driver_params) {
    return std::string(R"({"drivers":{"driver_dir":")") + TEST_DRIVER_DIR +
           R"(/simple","auto_discover":true,"load":[]},"resolver":{"use_custom_servers":false},)"
           R"("domains":[{"name":"yaddnsc.test","update_interval":60,"driver":"simple",)"
           R"("subdomains":[{"name":"iface","type":"a","ip_source":"interface","interface":")" + iface +
           R"(","driver_params":)" + driver_params + R"(}]}]})";
}

/// Any interface name known to the OS (loopback at minimum).
[[nodiscard]] std::string any_interface_name() {
    const SystemNetworkInterfaces interfaces;
    const auto names = interfaces.names();
    return names.empty() ? std::string{} : names.front();
}

/// Redirect the default spdlog logger into a string stream until destruction.
/// The run path reports through the logger, not stdout, so stream redirection
/// cannot see its diagnostics.
class LoggerCapture {
public:
    LoggerCapture() : previous_(spdlog::default_logger()) {
        const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream_);
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("composition-test", sink));
    }

    ~LoggerCapture() { spdlog::set_default_logger(previous_); }

    LoggerCapture(const LoggerCapture&) = delete;
    LoggerCapture& operator=(const LoggerCapture&) = delete;

    [[nodiscard]] std::string str() const { return stream_.str(); }

private:
    std::ostringstream stream_;
    std::shared_ptr<spdlog::logger> previous_;
};
}  // namespace

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

// Intentional behaviour: config show redacts sensitive driver_params fields by
// default. Rule: an object member is sensitive when its lower-cased key
// contains "token", "password", "secret" or "key"; the whole value is
// replaced with "***". Only fake placeholder values are used here — real
// tokens must never appear in golden files.
TEST(CliConfigTest, DispatchShow_RedactsSensitiveDriverParams) {
    const std::string config_json = R"({
        "drivers": {"auto_discover": false, "load": []},
        "resolver": {"use_custom_servers": false},
        "domains": [{
            "name": "example.com",
            "update_interval": 300,
            "driver": "simple",
            "subdomains": [{
                "name": "www",
                "type": "a",
                "ip_source": "http",
                "ip_source_param": "https://api.ipify.org",
                "driver_params": {
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

// Address fields are plain strings, so key-based redaction cannot see
// credentials embedded as URI userinfo; they are masked separately.
TEST(CliConfigTest, DispatchShow_RedactsUriCredentials) {
    const std::string config_json = R"({
        "drivers": {"auto_discover": false, "load": []},
        "resolver": {
            "use_custom_servers": true,
            "servers": [{"address": "https://fake-user:fake-pass@dns.example.net/dns-query", "port": 443}]
        },
        "domains": [{
            "name": "example.com",
            "update_interval": 300,
            "driver": "simple",
            "subdomains": [{
                "name": "www",
                "type": "a",
                "ip_source": "http",
                "ip_source_param": "https://fake-user:fake-pass@ifconfig.example.net/ip"
            }]
        }]
    })";
    TempConfigFile cfg(config_json);

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::ConfigShowCommand{cfg.path()}), EXIT_SUCCESS);
    const std::string out = capture.str();

    EXPECT_EQ(out.find("fake-user"), std::string::npos);
    EXPECT_EQ(out.find("fake-pass"), std::string::npos);
    EXPECT_NE(out.find("https://***@dns.example.net/dns-query"), std::string::npos);
    EXPECT_NE(out.find("https://***@ifconfig.example.net/ip"), std::string::npos);
}

TEST(CliConfigTest, DispatchTest_ValidConfig_ReturnsZero) {
    TempConfigFile cfg(config_with_simple_driver());
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_SUCCESS);
}

TEST(CliConfigTest, DispatchTest_AutoDiscoverValidConfig_ReturnsZero) {
    // auto_discover over the real driver directory, with a driver_params the
    // simple driver's own schema accepts.
    const auto iface = any_interface_name();
    ASSERT_FALSE(iface.empty()) << "no network interfaces available";
    TempConfigFile cfg(config_with_interface_source(iface, R"({"url":"http://127.0.0.1:1/ip?ip={ip_addr}"})"));

    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_SUCCESS);
}

TEST(CliConfigTest, DispatchTest_ValidConfig_PrintsPassed) {
    TempConfigFile cfg(config_with_simple_driver());

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("Configuration file test passed"), std::string::npos);
}

TEST(CliConfigTest, DispatchTest_Quiet_PrintsNothingAndPreservesLogLevel) {
    TempConfigFile cfg(config_with_simple_driver());

    const auto previous_level = spdlog::get_level();
    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path(), /*quiet=*/true}), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "");
    EXPECT_EQ(spdlog::get_level(), previous_level);
}

TEST(CliConfigTest, DispatchTest_QuietFailure_RestoresLogLevel) {
    // A quiet failure must still restore the global log level it lowered.
    const auto previous_level = spdlog::get_level();
    TempConfigFile cfg("{");
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path(), /*quiet=*/true}), EXIT_FAILURE);
    EXPECT_EQ(spdlog::get_level(), previous_level);
}

TEST(CliConfigTest, DispatchTest_EmptyDriverDir_ReturnsFailure) {
    // driver_dir set but empty → ConfigException at load time.
    TempConfigFile cfg(
        R"({"drivers":{"auto_discover":false,"driver_dir":"","load":["simple/simple.so"]},"resolver":{"use_custom_servers":false},"domains":[]})");
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_FAILURE);
}

TEST(CliConfigTest, DispatchTest_BadDriver_ReturnsFailure) {
    // A driver file that does not exist → PluginLoadException (YaddnscException).
    TempConfigFile cfg(config_bad_driver());
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_FAILURE);
}

TEST(CliConfigTest, DispatchTest_EmptyCustomResolverFailsBeforeDriverLoading) {
    // The deliberately missing driver must never be considered: static
    // resolver validation happens before catalog loading and environment
    // validation on every runtime path.
    const std::string config =
        std::string(R"({"drivers":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
        R"(","load":["definitely_missing_driver.so"]},"resolver":{"use_custom_servers":true},"domains":[]})";
    TempConfigFile cfg(config);

    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_FAILURE);
    EXPECT_NE(err.str().find("Configuration verification failed: use_custom_servers is enabled but no custom resolver "
                             "servers are configured"),
              std::string::npos);
}

TEST(CliConfigTest, DispatchTest_InterfaceMissing_ReturnsFailure) {
    // Environment validation: the interface the subdomain names does not
    // exist, so the config is rejected before any update runs.
    TempConfigFile cfg(config_with_interface_source("no-such-if0", R"({"url":"http://127.0.0.1:1/ip"})"));
    EXPECT_EQ(Composition::dispatch(Cli::ConfigTestCommand{cfg.path()}), EXIT_FAILURE);
}

TEST(CliConfigTest, DispatchTest_DriverParamRejectedByDriver_ReturnsFailure) {
    // The simple driver requires "url" in driver_params. An empty object must
    // be rejected through the driver's own validate entry point, which is the
    // host's way of catching a bad config before the first update.
    const auto iface = any_interface_name();
    ASSERT_FALSE(iface.empty()) << "no network interfaces available";
    TempConfigFile cfg(config_with_interface_source(iface, "{}"));

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
        std::string("{\"drivers\":{\"auto_discover\":false,\"driver_dir\":\"") + TEST_DRIVER_DIR +
        "\",\"load\":[\"simple/"
        "simple.so\"]},\"resolver\":{\"use_custom_servers\":false},\"domains\":[{\"name\":\"example.com\",\"update_"
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
//  Composition::dispatch — run
// ===========================================================================

TEST(CliRunTest, DispatchRun_EmptyCustomResolverFailsBeforeDriverLoading) {
    // Same guarantee as config test, on the run path: static resolver
    // validation precedes driver loading, resolver creation and scheduling,
    // so the deliberately missing driver must never be considered.
    const std::string config =
        std::string(R"({"drivers":{"auto_discover":false,"driver_dir":")") + TEST_DRIVER_DIR +
        R"(","load":["definitely_missing_driver.so"]},"resolver":{"use_custom_servers":true},"domains":[]})";
    TempConfigFile cfg(config);

    // A previous test may have lowered the global log level (config test
    // --quiet); the run path reports validation failures through the logger.
    const auto previous_level = spdlog::default_logger()->level();
    spdlog::set_level(spdlog::level::info);

    StreamCapture out{STDOUT_FILENO};
    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Composition::dispatch(Cli::RunCommand{cfg.path()}), EXIT_FAILURE);

    spdlog::set_level(previous_level);

    // The failure is reported as the resolver validation error (through the
    // logger), never as a driver load failure.
    const std::string logged = out.str() + err.str();
    EXPECT_NE(logged.find("use_custom_servers is enabled but no custom resolver servers are configured"),
              std::string::npos);
    EXPECT_EQ(logged.find("definitely_missing_driver"), std::string::npos);
}

TEST(CliRunTest, DispatchRun_InterfaceMissing_ReturnsFailure) {
    // Environment validation failure on the run path: the named interface
    // does not exist, so the run root never starts.
    TempConfigFile cfg(
        config_with_interface_source("no-such-if0", R"({"url":"http://127.0.0.1:1/ip?ip={ip_addr}"})"));
    EXPECT_EQ(Composition::dispatch(Cli::RunCommand{cfg.path()}), EXIT_FAILURE);
}

TEST(CliRunTest, DispatchRun_MultipleConfigErrors_AreAggregated) {
    // Two independent problems in one file. The run path joins every
    // collected message into a single critical line, so a user fixes the
    // whole config in one pass instead of rediscovering errors one run at
    // a time. Both subdomains are invalid, so no update can start.
    TempConfigFile cfg{R"({
  "drivers": { "auto_discover": false, "load": [] },
  "resolver": { "use_custom_servers": false },
  "domains": [
    {
      "name": "yaddnsc.test",
      "update_interval": 60,
      "driver": "simple",
      "subdomains": [
        {
          "name": "a",
          "type": "a",
          "ip_source": "interface",
          "interface": "",
          "driver_params": { "url": "http://127.0.0.1:1/ip" }
        },
        {
          "name": "b",
          "type": "a",
          "ip_source": "interface",
          "interface": "",
          "driver_params": { "url": "http://127.0.0.1:1/ip" }
        }
      ]
    }
  ]
}
)"};
    // The run handler returns EXIT_FAILURE for an invalid config and never
    // reaches the lifecycle, so this terminates.
    LoggerCapture logs;
    EXPECT_EQ(Composition::dispatch(Cli::RunCommand{cfg.path()}), EXIT_FAILURE);
    EXPECT_NE(logs.str().find("Subdomain a.yaddnsc.test uses interface IP source but 'interface' field is empty"),
              std::string::npos);
    EXPECT_NE(logs.str().find("Subdomain b.yaddnsc.test uses interface IP source but 'interface' field is empty"),
              std::string::npos);
}

TEST(CliRunTest, DispatchRun_MalformedConfig_ReportsConfigError) {
    // A config that cannot even be parsed is an expected startup failure: the
    // run handler logs the parse diagnostic and returns EXIT_FAILURE instead
    // of letting the exception escape dispatch().
    TempConfigFile cfg{R"({ "domains": [ )"};
    LoggerCapture logs;
    EXPECT_EQ(Composition::dispatch(Cli::RunCommand{cfg.path()}), EXIT_FAILURE);
    // The parse diagnostic names the offending file.
    EXPECT_NE(logs.str().find(cfg.path()), std::string::npos);
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

TEST(CliDriverTest, DispatchList_InvalidConfig_ReturnsFailure) {
    TempConfigFile cfg{"{ not json"};
    EXPECT_EQ(Composition::dispatch(Cli::DriverListCommand{cfg.path()}), EXIT_FAILURE);
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
    StdoutCapture out;
    EXPECT_EQ(Composition::dispatch(Cli::DriverInfoCommand{cfg.path(), "not_a_driver"}), EXIT_FAILURE);
    EXPECT_EQ(err.str(), "Error: Driver 'not_a_driver' is not loaded\n");
    EXPECT_EQ(out.str().find("Name:"), std::string::npos);
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
        R"({"drivers":{"auto_discover":false,"load":[]},"resolver":{"use_custom_servers":true,"servers":[{"address":"https://1.1.1.1/dns-query","port":443}]},"domains":[]})"};

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolverCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("https://1.1.1.1/dns-query"), std::string::npos);
}

TEST(CliDnsTest, DispatchResolver_BareServers_ReturnsZero) {
    TempConfigFile cfg{
        R"({"drivers":{"auto_discover":false,"load":[]},"resolver":{"use_custom_servers":true,"servers":[{"address":"8.8.8.8","port":53}]},"domains":[]})"};

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolverCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("8.8.8.8:53"), std::string::npos);
}

TEST(CliDnsTest, DispatchResolver_SingleEntryList_ReturnsZero) {
    TempConfigFile cfg{
        R"({"drivers":{"auto_discover":false,"load":[]},"resolver":{"use_custom_servers":true,"servers":[{"address":"9.9.9.9","port":53}]},"domains":[]})"};

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolverCommand{cfg.path()}), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("9.9.9.9:53"), std::string::npos);
}

TEST(CliDnsTest, DispatchResolver_MultipleServers_AreAllListed) {
    TempConfigFile cfg{R"({
  "drivers": { "auto_discover": false, "load": [] },
  "resolver": {
    "use_custom_servers": true,
    "servers": [
      { "address": "1.1.1.1", "port": 53 },
      { "address": "tls://8.8.8.8", "port": 853 },
      { "address": "https://dns.example/dns-query", "port": 443 }
    ]
  },
  "domains": []
}
)"};

    StdoutCapture capture;
    EXPECT_EQ(Composition::dispatch(Cli::DnsResolverCommand{cfg.path()}), EXIT_SUCCESS);
    const std::string out = capture.str();
    EXPECT_NE(out.find("1.1.1.1:53"), std::string::npos);
    EXPECT_NE(out.find("tls://8.8.8.8"), std::string::npos);
    EXPECT_NE(out.find("https://dns.example/dns-query"), std::string::npos);
}

TEST(CliDnsTest, DispatchResolver_UnparsableAddress_FallsBackToRaw) {
    // The display helper must never fail: a malformed address is shown as-is.
    TempConfigFile cfg{R"({
  "drivers": { "auto_discover": false, "load": [] },
  "resolver": {
    "use_custom_servers": true,
    "servers": [ { "address": "::not a uri::", "port": 53 } ]
  },
  "domains": []
}
)"};

    EXPECT_EQ(Composition::dispatch(Cli::DnsResolverCommand{cfg.path()}), EXIT_SUCCESS);
}

// The dns resolve dispatch path builds a real resolver dispatcher from the
// config, but an unknown record type short-circuits before any socket I/O:
// app::dns_resolve returns "no lookup" and the presenter reports the
// valid set. Command is constructed directly (the parser would reject the
// type), keeping the test on loopback-free, deterministic ground.
TEST(CliDnsTest, DispatchResolve_UnknownType_PrintsValidTypes) {
    // The dispatch path validates the config before resolving, so the file
    // must hold at least one statically-valid domain.
    TempConfigFile cfg{
        std::string(R"({"drivers":{"auto_discover":false,"load":[]},"resolver":{"use_custom_servers":false},)") +
        one_http_domain() + "}"};

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
