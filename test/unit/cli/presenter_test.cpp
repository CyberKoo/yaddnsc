//
// Unit tests for the CLI presenters (src/cli/presenter.*): stdout/stderr text
// and exit codes over hand-built outcomes — no dispatch, no I/O beyond the
// captured streams.
// =============================================================================

#include "cli/presenter.h"

#include <cstdlib>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

#include "application/diagnostics.h"
#include "application/ports/driver_catalog.h"
#include "domain/dns/record_kind.h"
#include "domain/driver/capabilities.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "fixtures/cli_test_support.h"

using namespace CliTestSupport;

TEST(CliPresenterTest, DnsResolve_UnknownType_PrintsValidTypes) {
    app::DnsResolveOutcome outcome{.host = "example.com", .type_text = "BOGUS", .lookup = std::nullopt};

    StreamCapture err{STDERR_FILENO};
    EXPECT_EQ(Cli::present_dns_resolve(outcome), EXIT_FAILURE);
    EXPECT_EQ(err.str(), "Error: unknown record type 'BOGUS'.\nValid types: A, AAAA, TXT\n");
}

TEST(CliPresenterTest, DnsResolve_Failure_PrintsMessageAndSucceeds) {
    app::DnsResolveOutcome outcome{.host = "example.com",
                                   .type_text = "A",
                                   .lookup = std::unexpected(domain::DnsErrorInfo{
                                       domain::DnsError::NX_DOMAIN, "Domain example.com does not exist (NXDOMAIN)"})};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolve(outcome), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "DNS lookup for example.com (A) failed: Domain example.com does not exist (NXDOMAIN)\n");
}

TEST(CliPresenterTest, DnsResolve_NoRecords_PrintsMessageAndSucceeds) {
    app::DnsResolveOutcome outcome{.host = "example.com", .type_text = "AAAA", .lookup = std::vector<std::string>{}};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolve(outcome), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "DNS lookup for example.com (AAAA) returned no records\n");
}

TEST(CliPresenterTest, DnsResolve_Records_PrintsResultBlock) {
    app::DnsResolveOutcome outcome{
        .host = "example.com", .type_text = "A", .lookup = std::vector<std::string>{"192.0.2.1", "192.0.2.2"}};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolve(outcome), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(), "DNS lookup result:\n  Host:  example.com\n  Type:  A\n  Value: 192.0.2.1, 192.0.2.2\n");
}

TEST(CliPresenterTest, DriverList_PerDriverFailure_PrintsInlineError) {
    std::vector<app::DriverListItem> items;
    items.push_back(app::DriverListItem{
        .name = "good",
        .detail = app::DriverDescription{.name = "good",
                                         .version = "1.0",
                                         .author = "a",
                                         .description = "d",
                                         .capabilities = domain::DriverCapabilities({domain::RecordKind::A}),
                                         .supports_validate = true}});
    items.push_back(app::DriverListItem{
        .name = "bad", .detail = std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND, {}})});

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_driver_list(items), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("bad — (failed to query details: Driver 'bad' is not loaded)"), std::string::npos);
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
    using Error = Cli::ConfigTestError;

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

// The resolver line is formatted from the view the composition root hands
// over: the endpoint was already parsed there, the CLI only spells it out.
TEST(CliPresenterTest, DnsResolver_SchemeWithPath_PrintsOriginAndPath) {
    const std::vector<Cli::DnsServerView> servers{{.original = "https://dns.example/dns-query",
                                                   .port = 443,
                                                   .endpoint = Cli::DnsServerEndpoint{.origin = "https://dns.example",
                                                                                      .host_literal = "dns.example",
                                                                                      .path = "/dns-query",
                                                                                      .has_scheme = true}}};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolver(true, "CONCURRENT", servers), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(),
              "DNS resolver configuration:\n"
              "  Custom server: yes\n"
              "  Strategy:      CONCURRENT\n"
              "  Servers (1):\n"
              "    - https://dns.example/dns-query\n");
}

TEST(CliPresenterTest, DnsResolver_SchemeWithRootPath_OmitsThePath) {
    // A scheme URI without an explicit path parses to "/", which is not
    // worth showing.
    const std::vector<Cli::DnsServerView> servers{
        {.original = "https://dns.example",
         .port = 443,
         .endpoint = Cli::DnsServerEndpoint{
             .origin = "https://dns.example", .host_literal = "dns.example", .path = "/", .has_scheme = true}}};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolver(true, "CONCURRENT", servers), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("    - https://dns.example\n"), std::string::npos);
}

TEST(CliPresenterTest, DnsResolver_BareAddress_PrintsHostAndPort) {
    // Without a scheme the endpoint has no origin of its own: the host and
    // the configured port are shown.
    const std::vector<Cli::DnsServerView> servers{
        {.original = "8.8.8.8",
         .port = 53,
         .endpoint =
             Cli::DnsServerEndpoint{.origin = "8.8.8.8", .host_literal = "8.8.8.8", .path = "", .has_scheme = false}}};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolver(true, "CONCURRENT", servers), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("    - 8.8.8.8:53\n"), std::string::npos);
}

TEST(CliPresenterTest, DnsResolver_UnparsableAddress_FallsBackToTheRawText) {
    // A malformed address has no endpoint: the display helper must never
    // fail, so the configured text is shown as-is.
    const std::vector<Cli::DnsServerView> servers{{.original = "::not a uri::", .port = 53, .endpoint = std::nullopt}};

    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolver(true, "CONCURRENT", servers), EXIT_SUCCESS);
    EXPECT_NE(capture.str().find("    - ::not a uri::\n"), std::string::npos);
}

TEST(CliPresenterTest, DnsResolver_WithoutServers_PrintsNoServerList) {
    StdoutCapture capture;
    EXPECT_EQ(Cli::present_dns_resolver(false, "FALLBACK", {}), EXIT_SUCCESS);
    EXPECT_EQ(capture.str(),
              "DNS resolver configuration:\n"
              "  Custom server: no\n"
              "  Strategy:      FALLBACK\n");
}
