//
// Component tests for the composition root
// (src/composition/bootstrap.cpp).
//
// Composition::dispatch() is the single place where concrete infrastructure
// is wired to the application ports, and it is the only entry point into
// every command. This file drives it directly through Cli::Command variants
// so the assembly and its failure contracts are covered without a shell:
//
//   - config test   static validation, environment validation, and the
//                   driver-side ABI validate_config hook
//   - dns resolver  server display formatting (URI vs raw address)
//   - driver list / info, interface list / ip, config show, info
//   - run           config errors are aggregated, not reported one at a time
//
// A real simple.so is loaded from the build tree, so driver diagnostics and
// environment validation exercise the actual plugin loader.
// =============================================================================

#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

#include <format>
#include <gtest/gtest.h>
#include <pthread.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include "cli/command.h"
#include "composition/bootstrap.h"
#include "infrastructure/process/signal_watcher.h"
#include "support/util/cancellation_token.hpp"

namespace {

namespace fs = std::filesystem;

/// Path to the driver built alongside the tests, or an empty string.
///
/// The composition root resolves drivers from the configured directory, so
/// these tests need the real module rather than a mock: `driver info` and
/// the environment validator both read metadata the loader exported.
[[nodiscard]] std::string simple_driver_dir() {
#ifdef YADDNSC_TEST_DRIVER_DIR
    // The CMake target passes the build-tree location of the driver as a
    // compile definition, so this is a compile-time constant.
    static const std::string dir{YADDNSC_TEST_DRIVER_DIR};
#else
    static const std::string dir{};
#endif
    return fs::exists(YADDNSC_TEST_DRIVER_FILE) ? dir : std::string{};
}

/// Write a config file and return its path.
[[nodiscard]] fs::path write_config(std::string_view name, std::string_view body) {
    const auto path = fs::temp_directory_path() / name;
    std::ofstream out(path, std::ios::trunc);
    out << body;
    out.close();
    return path;
}

void remove_file(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
}

/// Restores the calling thread's signal mask on scope exit.
///
/// run_command() calls SignalWatcher::install(), which blocks SIGINT/SIGTERM
/// for the calling thread permanently. Left alone that would silence Ctrl-C
/// for every later case in this binary, so the mask is captured and restored.
class ScopedSignalMask {
public:
    ScopedSignalMask() {
        if (::pthread_sigmask(SIG_BLOCK, nullptr, &saved_) != 0) {
            throw std::runtime_error("pthread_sigmask failed");
        }
    }

    ~ScopedSignalMask() {
        // The watcher is already joined. Only its reserved wake-up signal is
        // consumed; user SIGINT/SIGTERM and previously blocked signals retain
        // their normal semantics. sigpending + sigwait works on macOS too.
        sigset_t wake;
        ::sigemptyset(&wake);
        ::sigaddset(&wake, SIGUSR2);
        if (::pthread_sigmask(SIG_BLOCK, &wake, nullptr) != 0) {
            std::terminate();
        }
        sigset_t pending;
        if (::sigpending(&pending) != 0) {
            std::terminate();
        }
        if (::sigismember(&pending, SIGUSR2) == 1 && ::sigismember(&saved_, SIGUSR2) == 0) {
            int signal = 0;
            if (::sigwait(&wake, &signal) != 0) {
                std::terminate();
            }
        }
        if (::pthread_sigmask(SIG_SETMASK, &saved_, nullptr) != 0) {
            std::terminate();
        }
    }

    ScopedSignalMask(const ScopedSignalMask&) = delete;
    ScopedSignalMask& operator=(const ScopedSignalMask&) = delete;

private:
    sigset_t saved_{};
};

TEST(CompositionSignalMask, RestoreMask_DrainsPendingWakeSignal) {
    sigset_t before;
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, nullptr, &before), 0);
    {
        const ScopedSignalMask restore_signals;
        SignalWatcher::install();
        ASSERT_EQ(::kill(::getpid(), SIGUSR2), 0);
    }
    sigset_t after;
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, nullptr, &after), 0);
    for (const int signal : {SIGINT, SIGTERM, SIGUSR2}) {
        EXPECT_EQ(::sigismember(&after, signal), ::sigismember(&before, signal));
    }
}

std::string loopback_iface() {
    // "lo" on Linux, "lo0" on macOS/BSD. An interface source config is the
    // simplest way to exercise the happy path without network access.
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    return "lo0";
#else
    return "lo";
#endif
}

// A config whose driver_dir points at the real built driver and which passes
// every validation stage: a domain, a resolvable interface, and a driver_param
// the simple driver's own schema accepts.
std::string valid_config() {
    return std::format(R"({{
  "driver": {{
    "driver_dir": "{}",
    "auto_discover": true,
    "load": []
  }},
  "resolver": {{ "use_custom_server": false }},
  "domains": [
    {{
      "name": "yaddnsc.test",
      "update_interval": 60,
      "driver": "simple",
      "subdomains": [
        {{
          "name": "iface",
          "type": "a",
          "ip_source": "interface",
          "ip_type": "ipv4",
          "interface": "{}",
          "driver_param": {{ "url": "http://127.0.0.1:1/ip?ip={{ip_addr}}" }}
        }}
      ]
    }}
  ]
}}
)",
                       simple_driver_dir(), loopback_iface());
}

// ---------------------------------------------------------------------------
// info — the only command that touches no configuration at all
// ---------------------------------------------------------------------------

TEST(CompositionSignalMask, RestoreMask_PreservesBlockedUserSignal) {
    const ScopedSignalMask restore_original;
    sigset_t user_signal;
    ::sigemptyset(&user_signal);
    ::sigaddset(&user_signal, SIGTERM);
    ASSERT_EQ(::pthread_sigmask(SIG_BLOCK, &user_signal, nullptr), 0);
    ASSERT_EQ(::kill(::getpid(), SIGTERM), 0);
    {
        const ScopedSignalMask restore_signals;
        SignalWatcher::install();
        ASSERT_EQ(::kill(::getpid(), SIGUSR2), 0);
    }
    sigset_t pending;
    ASSERT_EQ(::sigpending(&pending), 0);
    EXPECT_EQ(::sigismember(&pending, SIGTERM), 1);
    if (::sigismember(&pending, SIGTERM) == 1) {
        int signal = 0;
        EXPECT_EQ(::sigwait(&user_signal, &signal), 0);
        EXPECT_EQ(signal, SIGTERM);
    }
}

TEST(CompositionDispatch, RunCommand_EnvironmentFailure_JoinsWatcherBeforeMaskRestore) {
    auto config = valid_config();
    const auto iface = loopback_iface();
    const auto offset = config.find("\"interface\": \"" + iface + "\"");
    ASSERT_NE(offset, std::string::npos);
    config.replace(offset, std::string("\"interface\": \"" + iface + "\"").size(), "\"interface\": \"no-such-if0\"");
    const auto path = write_config("yaddnsc-compose-run-env.json", config);
    const ScopedSignalMask restore_signals;
    EXPECT_EQ(Composition::dispatch(Cli::Command{Cli::RunCommand{.config_path = path.string()}}), EXIT_FAILURE);
    remove_file(path);
}

TEST(CompositionDispatch, InfoCommand_Succeeds) {
    EXPECT_EQ(Composition::dispatch(Cli::Command{Cli::InfoCommand{}}), 0);
}

TEST(CompositionDispatch, InterfaceListCommand_Succeeds) {
    EXPECT_EQ(Composition::dispatch(Cli::Command{Cli::InterfaceListCommand{}}), 0);
}

TEST(CompositionDispatch, InterfaceIpCommand_UnknownInterface_Fails) {
    // The presenter turns an empty address list into a non-zero exit.
    const Cli::Command command{Cli::InterfaceIpCommand{.name = "definitely-not-an-interface"}};
    EXPECT_NE(Composition::dispatch(command), 0);
}

// ---------------------------------------------------------------------------
// config show / test
// ---------------------------------------------------------------------------

TEST(CompositionDispatch, ConfigShowCommand_EmitsJson) {
    const auto path = write_config("yaddnsc-compose-show.json", valid_config());
    const Cli::Command command{Cli::ConfigShowCommand{.config_path = path.string()}};
    EXPECT_EQ(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, ConfigTestCommand_ValidConfig_Passes) {
    const auto path = write_config("yaddnsc-compose-test-ok.json", valid_config());
    const Cli::Command command{Cli::ConfigTestCommand{.config_path = path.string(), .quiet = false}};
    EXPECT_EQ(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, ConfigTestCommand_Quiet_StillPasses) {
    // `config test --quiet` sets the *process-wide* spdlog level to off and
    // never restores it, so this test must put it back or it silences logging
    // for every case that runs after it in this binary.
    const auto previous_level = spdlog::get_level();
    const auto path = write_config("yaddnsc-compose-test-quiet.json", valid_config());
    const Cli::Command command{Cli::ConfigTestCommand{.config_path = path.string(), .quiet = true}};
    const auto rc = Composition::dispatch(command);
    spdlog::set_level(previous_level);
    EXPECT_EQ(rc, 0);
    remove_file(path);
}

TEST(CompositionDispatch, ConfigTestCommand_InvalidConfig_Fails) {
    // Truncated JSON: the parse error is collected, not thrown.
    const auto path = write_config("yaddnsc-compose-test-bad.json", R"({ "driver": { "driver_dir": )");
    const Cli::Command command{Cli::ConfigTestCommand{.config_path = path.string(), .quiet = false}};
    EXPECT_NE(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, ConfigTestCommand_EmptyDriverDir_Fails) {
    // driver_dir set but empty throws ConfigVerificationException, which the
    // config-test handler must present as a verification error rather than
    // letting it escape as a fatal.
    const auto path = write_config("yaddnsc-compose-test-emptydir.json", R"({
  "driver": { "driver_dir": "", "auto_discover": true, "load": [] },
  "resolver": { "use_custom_server": false },
  "domains": []
}
)");
    const Cli::Command command{Cli::ConfigTestCommand{.config_path = path.string(), .quiet = false}};
    EXPECT_NE(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, ConfigTestCommand_ReferencedInterfaceMissing_Fails) {
    // Environment validation: the interface the subdomain names does not
    // exist, so the config is rejected before any update runs.
    const auto path = write_config("yaddnsc-compose-test-badiface.json", std::format(R"({{
  "driver": {{ "driver_dir": "{}", "auto_discover": true, "load": [] }},
  "resolver": {{ "use_custom_server": false }},
  "domains": [
    {{
      "name": "yaddnsc.test",
      "update_interval": 60,
      "driver": "simple",
      "subdomains": [
        {{
          "name": "iface",
          "type": "a",
          "ip_source": "interface",
          "ip_type": "ipv4",
          "interface": "no-such-if0",
          "driver_param": {{ "url": "http://127.0.0.1:1/ip" }}
        }}
      ]
    }}
  ]
}}
)",
                                                                                     simple_driver_dir()));
    const Cli::Command command{Cli::ConfigTestCommand{.config_path = path.string(), .quiet = false}};
    EXPECT_NE(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, ConfigTestCommand_DriverParamRejectedByAbi_Fails) {
    // The simple driver requires "url" in driver_param. An empty object must
    // be rejected through the driver's own validate entry point, which is the
    // host's way of catching a bad config before the first update.
    const auto path =
        write_config("yaddnsc-compose-test-badparam.json", std::format(R"({{
  "driver": {{ "driver_dir": "{}", "auto_discover": true, "load": [] }},
  "resolver": {{ "use_custom_server": false }},
  "domains": [
    {{
      "name": "yaddnsc.test",
      "update_interval": 60,
      "driver": "simple",
      "subdomains": [
        {{
          "name": "iface",
          "type": "a",
          "ip_source": "interface",
          "ip_type": "ipv4",
          "interface": "{}",
          "driver_param": {{}}
        }}
      ]
    }}
  ]
}}
)",
                                                                       simple_driver_dir(), loopback_iface()));
    const Cli::Command command{Cli::ConfigTestCommand{.config_path = path.string(), .quiet = false}};
    EXPECT_NE(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, ConfigTestCommand_ValidDriverParam_Passes) {
    const auto path =
        write_config("yaddnsc-compose-test-goodparam.json", std::format(R"({{
  "driver": {{ "driver_dir": "{}", "auto_discover": true, "load": [] }},
  "resolver": {{ "use_custom_server": false }},
  "domains": [
    {{
      "name": "yaddnsc.test",
      "update_interval": 60,
      "driver": "simple",
      "subdomains": [
        {{
          "name": "iface",
          "type": "a",
          "ip_source": "interface",
          "ip_type": "ipv4",
          "interface": "{}",
          "driver_param": {{ "url": "http://127.0.0.1:1/ip?ip={{ip_addr}}" }}
        }}
      ]
    }}
  ]
}}
)",
                                                                        simple_driver_dir(), loopback_iface()));
    const Cli::Command command{Cli::ConfigTestCommand{.config_path = path.string(), .quiet = false}};
    EXPECT_EQ(Composition::dispatch(command), 0);
    remove_file(path);
}

// ---------------------------------------------------------------------------
// driver diagnostics — load the real module
// ---------------------------------------------------------------------------

TEST(CompositionDispatch, DriverListCommand_LoadsBuiltDriver) {
    if (simple_driver_dir().empty()) {
        GTEST_SKIP() << "simple.so not found; set YADDNSC_TEST_DRIVER_DIR";
    }
    const auto path = write_config("yaddnsc-compose-drivers.json", valid_config());
    const Cli::Command command{Cli::DriverListCommand{.config_path = path.string()}};
    EXPECT_EQ(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, DriverInfoCommand_KnownDriver_Succeeds) {
    if (simple_driver_dir().empty()) {
        GTEST_SKIP() << "simple.so not found; set YADDNSC_TEST_DRIVER_DIR";
    }
    const auto path = write_config("yaddnsc-compose-driverinfo.json", valid_config());
    const Cli::Command command{Cli::DriverInfoCommand{.config_path = path.string(), .name = "simple"}};
    EXPECT_EQ(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, DriverInfoCommand_UnknownDriver_Fails) {
    if (simple_driver_dir().empty()) {
        GTEST_SKIP() << "simple.so not found; set YADDNSC_TEST_DRIVER_DIR";
    }
    const auto path = write_config("yaddnsc-compose-driverinfo-bad.json", valid_config());
    const Cli::Command command{Cli::DriverInfoCommand{.config_path = path.string(), .name = "no-such-driver"}};
    EXPECT_NE(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, DriverListCommand_InvalidConfig_Fails) {
    const auto path = write_config("yaddnsc-compose-drivers-bad.json", R"({ not json )");
    const Cli::Command command{Cli::DriverListCommand{.config_path = path.string()}};
    EXPECT_NE(Composition::dispatch(command), 0);
    remove_file(path);
}

// ---------------------------------------------------------------------------
// dns resolver — server display formatting
// ---------------------------------------------------------------------------

TEST(CompositionDispatch, DnsResolverCommand_PlainAddress_IsRendered) {
    const auto path = write_config("yaddnsc-compose-resolver-plain.json", R"({
  "driver": { "auto_discover": false, "load": [] },
  "resolver": {
    "use_custom_server": true,
    "servers": [ { "address": "1.1.1.1", "port": 53 } ]
  },
  "domains": []
}
)");
    const Cli::Command command{Cli::DnsResolverCommand{.config_path = path.string()}};
    EXPECT_EQ(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, DnsResolverCommand_UriAddressWithPath_IsRendered) {
    // A scheme-bearing address is displayed as origin + path, not host:port.
    const auto path = write_config("yaddnsc-compose-resolver-uri.json", R"({
  "driver": { "auto_discover": false, "load": [] },
  "resolver": {
    "use_custom_server": true,
    "servers": [ { "address": "https://dns.example/dns-query", "port": 443 } ]
  },
  "domains": []
}
)");
    const Cli::Command command{Cli::DnsResolverCommand{.config_path = path.string()}};
    EXPECT_EQ(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, DnsResolverCommand_UnparsableAddress_FallsBackToRaw) {
    // The display helper must never fail: a malformed address is shown as-is.
    const auto path = write_config("yaddnsc-compose-resolver-raw.json", R"({
  "driver": { "auto_discover": false, "load": [] },
  "resolver": {
    "use_custom_server": true,
    "servers": [ { "address": "::not a uri::", "port": 53 } ]
  },
  "domains": []
}
)");
    const Cli::Command command{Cli::DnsResolverCommand{.config_path = path.string()}};
    EXPECT_EQ(Composition::dispatch(command), 0);
    remove_file(path);
}

TEST(CompositionDispatch, DnsResolverCommand_MultipleServers_AreAllListed) {
    const auto path = write_config("yaddnsc-compose-resolver-multi.json", R"({
  "driver": { "auto_discover": false, "load": [] },
  "resolver": {
    "use_custom_server": true,
    "servers": [
      { "address": "1.1.1.1", "port": 53 },
      { "address": "tls://8.8.8.8", "port": 853 },
      { "address": "https://dns.example/dns-query", "port": 443 }
    ]
  },
  "domains": []
}
)");
    const Cli::Command command{Cli::DnsResolverCommand{.config_path = path.string()}};
    EXPECT_EQ(Composition::dispatch(command), 0);
    remove_file(path);
}

// ---------------------------------------------------------------------------
// run — configuration errors must be aggregated, not reported one at a time
// ---------------------------------------------------------------------------

TEST(CompositionDispatch, RunCommand_MultipleConfigErrors_AreAggregated) {
    // Two independent problems in one file. The run path joins every
    // collected message into a single critical line, so a user fixes the
    // whole config in one pass instead of rediscovering errors one run at
    // a time. Both subdomains are invalid, so no update can start.
    const auto path = write_config("yaddnsc-compose-run-bad.json", R"({
  "driver": { "auto_discover": false, "load": [] },
  "resolver": { "use_custom_server": false },
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
          "ip_type": "ipv4",
          "interface": "",
          "driver_param": { "url": "http://127.0.0.1:1/ip" }
        },
        {
          "name": "b",
          "type": "a",
          "ip_source": "interface",
          "ip_type": "ipv4",
          "interface": "",
          "driver_param": { "url": "http://127.0.0.1:1/ip" }
        }
      ]
    }
  ]
}
)");
    // run_command returns EXIT_FAILURE for an invalid config, and never
    // reaches the lifecycle, so this terminates.
    const Cli::Command command{Cli::RunCommand{.config_path = path.string(), .verbose = false}};
    const ScopedSignalMask restore_signals;
    std::ostringstream diagnostics;
    const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(diagnostics);
    const auto logger = std::make_shared<spdlog::logger>("composition-test", sink);
    const auto previous = spdlog::default_logger();

    struct RestoreLogger {
        std::shared_ptr<spdlog::logger> previous;

        ~RestoreLogger() noexcept { spdlog::set_default_logger(previous); }
    } restore_logger{previous};

    spdlog::set_default_logger(logger);
    EXPECT_EQ(Composition::dispatch(command), EXIT_FAILURE);
    const auto output = diagnostics.str();
    EXPECT_NE(output.find("Subdomain a.yaddnsc.test uses interface IP source but 'interface' field is empty"),
              std::string::npos);
    EXPECT_NE(output.find("Subdomain b.yaddnsc.test uses interface IP source but 'interface' field is empty"),
              std::string::npos);
    remove_file(path);
}

TEST(CompositionDispatch, RunCommand_MalformedConfig_ThrowsToMainBoundary) {
    // Unlike the diagnostic commands, `run` has no catch inside dispatch():
    // bootstrap.h states that exceptions escape so main() can turn them into
    // fatal log lines. A config that cannot even be parsed therefore throws
    // rather than returning a code.
    const auto path = write_config("yaddnsc-compose-run-malformed.json", R"({ "domains": [ )");
    const Cli::Command command{Cli::RunCommand{.config_path = path.string(), .verbose = false}};
    const ScopedSignalMask restore_signals;
    EXPECT_ANY_THROW(static_cast<void>(Composition::dispatch(command)));
    remove_file(path);
}

}  // namespace
