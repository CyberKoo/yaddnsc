// Exercise the actual CMake guard against isolated source trees.
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include "component/process_test_support.h"

namespace {

struct GuardCase {
    const char* name;
    const char* source;
    bool allowed;
    const char* path{"src/application/probe.cpp"};
    const char* expected_diagnostic{nullptr};
};

class ApplicationCoroGuard : public ::testing::TestWithParam<GuardCase> {};

TEST_P(ApplicationCoroGuard, Check_Source_EnforcesPublicBoundary) {
    const GuardCase& test = GetParam();
    ComponentTest::TempDirectory dir{(std::filesystem::temp_directory_path() / "yaddnsc-guard-XXXXXX").string()};
    std::filesystem::create_directories((dir.path() / test.path).parent_path());
    std::filesystem::create_directories(dir.path() / "include/yaddnsc/sdk");
    // Other architecture checks require the SDK header to exist.
    std::ofstream(dir.path() / "include/yaddnsc/sdk/driver_abi.h") << "#include <stdint.h>\n";
    std::filesystem::create_directories(dir.path() / "src/infrastructure/probe");
    std::ofstream(dir.path() / "src/infrastructure/probe/types.h") << R"(
// class CommentOnlyBackend {};
/* struct AnotherCommentOnlyBackend {}; */
namespace domain { class InetAddress; }
namespace app { class NetworkInterfacesPort; }
class NetworkInterfaces {};
class RenamedBackend final : public app::NetworkInterfacesPort {};
struct BackendState {};
class BorrowedForwardOnly;
namespace net { class Stream {}; }
namespace Config { struct SubdomainConfig {}; }
)";
    std::ofstream(dir.path() / test.path) << test.source;
    const auto output = dir.path() / "output.txt";
    const std::string root_argument = "-DPROJECT_SOURCE_DIR=" + dir.path().string();

    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        if (std::freopen(output.c_str(), "w", stdout) == nullptr || ::dup2(STDOUT_FILENO, STDERR_FILENO) == -1) {
            ::_exit(126);
        }
        ::execl(YADDNSC_TEST_CMAKE, YADDNSC_TEST_CMAKE, root_argument.c_str(), "-P", YADDNSC_TEST_GUARD, nullptr);
        ::_exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited == -1 && errno == EINTR);
    ASSERT_EQ(waited, pid);
    ASSERT_TRUE(WIFEXITED(status));
    const int exit_code = WEXITSTATUS(status);
    ASSERT_NE(exit_code, 126);
    ASSERT_NE(exit_code, 127);
    std::ifstream stream{output};
    const std::string diagnostic{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    EXPECT_EQ(exit_code == 0, test.allowed) << diagnostic;
    if (!test.allowed) {
        EXPECT_NE(diagnostic.find(test.path), std::string::npos) << diagnostic;
    }
    if (test.expected_diagnostic != nullptr) {
        EXPECT_NE(diagnostic.find(test.expected_diagnostic), std::string::npos) << diagnostic;
    }
}

INSTANTIATE_TEST_SUITE_P(
    PublicApi, ApplicationCoroGuard,
    ::testing::Values(
        GuardCase{"PublicHeaders", R"(#include "infrastructure/coro/task.hpp"
#include "infrastructure/coro/group.hpp"
#include "infrastructure/coro/scope.hpp"
#include "infrastructure/coro/now.hpp"
#include "infrastructure/coro/time.h"
#include "infrastructure/coro/fwd.h"
#include "infrastructure/coro/cancel_scope.h"
#include "infrastructure/coro/sleep.hpp"
#include "infrastructure/coro/cancelled.h"
#include "infrastructure/coro/checkpoint.hpp"
#include "infrastructure/coro/signal.hpp"
#include "infrastructure/coro/offload.hpp"
#include "infrastructure/coro/async_mutex.hpp"
)",
                  true},
        GuardCase{"PortContractsAndAggregation", R"(#ifndef YADDNSC_APPLICATION_PORTS_PROBE_H
#define YADDNSC_APPLICATION_PORTS_PROBE_H
#include <expected>
#include "domain/config/runtime.h"
#include "application/ports/log.h"
#include <application/ports/network_interfaces.h>
#include "infrastructure/coro/task.hpp"
#include <infrastructure/coro/cancel_scope.h>
namespace app {
class ProbePort { public: virtual ~ProbePort() = default; };
class OtherPort { public: virtual ~OtherPort() = default; };
struct ProbePorts { ProbePort& probe; OtherPort& other; };
}
#endif // YADDNSC_APPLICATION_PORTS_PROBE_H
)",
                  true, "src/application/ports/probe.h"},
        GuardCase{"PortFmtHeader", "#include <fmt/format.h>\n", false, "src/application/ports/log.h",
                  "must not include formatting implementations"},
        GuardCase{"PortQuotedFmtHeader", "#include \"fmt/core.h\"\n", false, "src/application/ports/probe.hpp"},
        GuardCase{"PortStandardFormat", "#include <format>\n", false, "src/application/ports/probe.h",
                  "must not include formatting implementations"},
        GuardCase{"PortSupportFmt", "#include \"support/fmt.hpp\"\n", false, "src/application/ports/log.h",
                  "port contracts may include only"},
        GuardCase{"PortAngleSupportFmt", "#include <support/fmt.hpp>\n", false, "src/application/ports/probe.h"},
        GuardCase{"PortLoggingConvenience", "#include \"application/log.h\"\n", false, "src/application/ports/log.h",
                  "port contracts may include only"},
        GuardCase{"PortApplicationHeader", "#include \"application/services.h\"\n", false,
                  "src/application/ports/probe.h"},
        GuardCase{"PortSupportHelper", "#include \"support/string_util.hpp\"\n", false,
                  "src/application/ports/nested/probe.hpp"},
        GuardCase{"PortBareInternalHeader", "#include \"log.h\"\n", false, "src/application/ports/probe.h"},
        GuardCase{"PortRelativeConvenience", "#include \"../log.h\"\n", false, "src/application/ports/probe.h"},
        GuardCase{"PortTraversalConvenience", "#include \"application/ports/../log.h\"\n", false,
                  "src/application/ports/probe.h"},
        GuardCase{"PortDomainTraversal", "#include <domain/../support/fmt.hpp>\n", false,
                  "src/application/ports/probe.h"},
        GuardCase{"PortInternalCoro", "#include \"infrastructure/coro/loop.h\"\n", false,
                  "src/application/ports/probe.h", "port contracts may include only"},
        GuardCase{"PortFunctionMacro", "#define LOG_AT(level, ...) log(level, __VA_ARGS__)\n", false,
                  "src/application/ports/log.h", "must not define function-like macros"},
        GuardCase{"PortEmptyFunctionMacro", "# define HELPER() 0\n", false, "src/application/ports/nested/probe.hpp",
                  "must not define function-like macros"},
        GuardCase{"PortObjectMacrosAndComments", R"(#ifndef PROBE_H
#define PROBE_H
#define DEFAULT_VALUE (1)
// #define HELPER(x) (x)
/* #define HELPER() 0 */
// #include "application/log.h"
#endif
)",
                  true, "src/application/ports/probe.h"},
        GuardCase{"ApplicationSupportStillAllowed", R"(#include "support/fmt.hpp"
#include "support/string_util.hpp"
#include "application/log.h"
#include <fmt/format.h>
#include <format>
#define HELPER(x) (x)
)",
                  true, "src/application/probe.h"},
        GuardCase{"LoggingConvenienceMacroStillAllowed", "#define LOG_AT(level, ...) log(level, __VA_ARGS__)\n", true,
                  "src/application/log.h"},
        GuardCase{"LegacyGlobalForward", "class NetworkInterfaces; void inspect(NetworkInterfaces&);", false,
                  "src/application/diagnostics.h", "concrete type NetworkInterfaces"},
        GuardCase{"RenamedGlobalForward", "class\n RenamedBackend\n;", false, "src/application/probe.hpp",
                  "concrete type RenamedBackend"},
        GuardCase{"GlobalStructForward", "struct BackendState;", false, "src/application/probe.cpp",
                  "concrete type BackendState"},
        GuardCase{"GlobalForwardAfterUrl", "auto u = \"https://example.org\"; class RenamedBackend;", false},
        GuardCase{"GlobalForwardWithComments", "class /* explanation */ RenamedBackend /* split */;", false},
        GuardCase{"InfrastructureType", "net::Stream* stream;", false, "src/application/probe.cpp",
                  "infrastructure namespaces"},
        GuardCase{"InfrastructureTypeAlias", "using Stream = ::net::Stream;", false},
        GuardCase{"InfrastructureTypedef", "typedef net::Stream Stream;", false},
        GuardCase{"InfrastructureNamespaceAlias", "namespace n = ::net; namespace other = n;", false},
        GuardCase{"InfrastructureNestedAlias", "namespace n = net::detail;", false},
        GuardCase{"InfrastructureImport", "using namespace ::net;", false},
        GuardCase{"InfrastructureUnqualifiedImport", "using namespace net;", false},
        GuardCase{"InfrastructureUsingType", "using net::Stream;", false},
        GuardCase{"InfrastructureForward", "namespace net { class Stream; }", false},
        GuardCase{"InfrastructureNestedForward", "namespace net::detail { struct Stream; }", false},
        GuardCase{"InfrastructureSplitQualifier", "::net\n :: Stream* stream;", false},
        GuardCase{"DnsType", "dns::Resolver* resolver;", false}, GuardCase{"HttpType", "http::Client* client;", false},
        GuardCase{"IpSourceType", "ipsource::InterfaceIpSource* source;", false},
        GuardCase{"ConfigType", "Config::AppConfig* config;", false},
        GuardCase{"LoggingAlias", "namespace backend = logging;", false},
        GuardCase{"PluginType", "plugin::Loader* loader;", false},
        GuardCase{"InfrastructureRoot", "infrastructure::Backend* backend;", false},
        GuardCase{"CertificateHelper", "Utils::Cert::load();", false},
        GuardCase{"ReasonableForwards", R"(class ApplicationHelper;
struct ApplicationState;
class BorrowedForwardOnly;
namespace domain { class InetAddress; struct RuntimeConfig; }
namespace app { class NetworkInterfacesPort; class LoggerPort; struct Services; }
namespace coro { template<class T> class Task; class CancelScope; }
namespace a = app; namespace d = domain;
using Port = a::NetworkInterfacesPort;
coro::Task<void> inspect(Port&, d::InetAddress&);
)",
                  true, "src/application/probe.h"},
        GuardCase{"ApplicationSameNameForward", "namespace app { class Stream; }", true},
        GuardCase{"DomainConfigSameNameForward", R"(namespace domain {
struct SubdomainConfig;
}
namespace app {
class IpSourcePort { public: virtual void read(const domain::SubdomainConfig&) = 0; };
}
)",
                  true, "src/application/ports/ip_source.h"},
        GuardCase{"NestedBusinessSameNameForwards", R"(namespace domain::detail { struct SubdomainConfig; }
namespace app { namespace detail { class Stream; } class RenamedBackend; }
)",
                  true},
        GuardCase{"BusinessForwardDoesNotMaskGlobal", R"(namespace domain { struct SubdomainConfig; }
namespace app { class RenamedBackend; }
class RenamedBackend;
)",
                  false, "src/application/probe.h", "concrete type RenamedBackend"},
        GuardCase{"GlobalForwardBetweenBlocks", R"(namespace app { class Stream; }
struct BackendState;
namespace domain { struct SubdomainConfig; }
)",
                  false, "src/application/probe.h", "concrete type BackendState"},
        GuardCase{"SameNameDefinitionIsNotForward", "class Stream { struct Nested {}; };", true},
        GuardCase{"InfrastructureReferenceInsideBusinessBlock", "namespace app { net::Stream* stream; }", false,
                  "src/application/probe.h", "infrastructure namespaces"},
        GuardCase{"InfrastructureGlobalImportAlias", "namespace n = ::infrastructure;", false},
        GuardCase{"CommentOnlyDefinitions", "class CommentOnlyBackend; struct AnotherCommentOnlyBackend;", true},
        GuardCase{"UnrelatedNames", "class RenamedBackendExtra; struct BackendStateView; namespace network {}", true},
        GuardCase{"InfrastructureCommentsAndStrings", R"(// namespace n = net;
/* namespace net { class Stream; } class NetworkInterfaces; */
auto note = "net::Stream; class NetworkInterfaces;";
)",
                  true},
        GuardCase{"CompositionMayUseConcreteTypes", "namespace n = net; class RenamedBackend;", true,
                  "src/composition/probe.cpp"},
        GuardCase{"PublicCancellation", "void stop(coro::TaskGroup& g) { g.scope().cancel(); g.cancel(); }", true},
        GuardCase{"PublicState", "bool read(coro::CancelScope& s) { return s.cancelled() || s.timed_out(); }", true},
        GuardCase{"UnrelatedRelease", "auto result = file.release(); auto other = tree.parent();", true},
        GuardCase{"PublicAlias", "namespace c = ::coro; c::Task<void> f();", true},
        GuardCase{"TransportIncludesTls", "#include \"infrastructure/network/tls/stream.h\"\n", false,
                  "src/infrastructure/network/transport/probe.cpp"},
        GuardCase{"TransportIncludesFactory", "#include \"infrastructure/network/factory/default_stream_factory.h\"\n",
                  false, "src/infrastructure/network/transport/probe.cpp"},
        GuardCase{"FactoryIncludesTls", "#include \"infrastructure/network/tls/stream.h\"\n", true,
                  "src/infrastructure/network/factory/probe.cpp"},
        GuardCase{"HttpIncludesDns", "#include \"infrastructure/dns/bootstrap/bootstrap.h\"\n", false,
                  "src/infrastructure/http/probe.cpp"},
        GuardCase{"HttpIncludesResolver", "#include \"infrastructure/network/address/resolver.h\"\n", true,
                  "src/infrastructure/http/probe.cpp"},
        GuardCase{"Comments", "/* coro::detail::GetContext c;\n coro::Loop loop; */\n// coro::GetContext{}\n", true},
        // Regression: "//" inside a string literal is not a comment. Stripping
        // comments before string literals truncated the rest of the line and
        // silently disabled every symbol check on it, so a runtime type sitting
        // next to an endpoint literal passed the guard.
        GuardCase{"UrlLiteralDoesNotMaskRuntimeType",
                  "static constexpr const char* kHost = \"https://dns.google\"; coro::Loop loop;", false},
        GuardCase{"UrlLiteralOnItsOwnLineIsFine",
                  "static constexpr const char* kHost = \"https://dns.google/dns-query\";\n", true},
        GuardCase{"CommentedCodeAfterUrlIsStillExempt",
                  "auto u = \"https://dns.google\";\n// coro::Loop loop;\n/* coro::detail::GetContext */\n", true},
        GuardCase{"LoopHeader", "#include \"infrastructure/coro/loop.h\"\n", false},
        GuardCase{"ClockHeader", "#include \"infrastructure/coro/clock.h\"\n", false},
        GuardCase{"FrameHeader", "#include \"infrastructure/coro/detail/frame.h\"\n", false},
        GuardCase{"FdHeader", "#include \"infrastructure/coro/fd_wait.hpp\"\n", false},
        GuardCase{"ResultBoxHeader", "#include \"infrastructure/coro/detail/result_box.hpp\"\n", false},
        GuardCase{"WaitNodeHeader", "#include \"infrastructure/coro/detail/wait_node.h\"\n", false},
        GuardCase{"TimerNodeHeader", "#include \"infrastructure/coro/detail/timer_node.h\"\n", false},
        GuardCase{"RunHeader", "#include \"infrastructure/coro/run.hpp\"\n", false},
        GuardCase{"UmbrellaHeader", "#include \"infrastructure/coro/coro.h\"\n", false},
        GuardCase{"Context", "auto c = co_await coro::GetContext{};", false},
        GuardCase{"InternalContext", "auto c = co_await coro::detail::GetContext{};", false},
        GuardCase{"LoopObject", "coro::Loop loop;", false}, GuardCase{"ClockObject", "coro::ManualClock clock;", false},
        GuardCase{"FrameType", "coro::PromiseBase* frame;", false},
        GuardCase{"WaitType", "coro::WaitNode node;", false},
        GuardCase{"Alias", "namespace c = coro; auto c = co_await c::detail::GetContext{};", false},
        GuardCase{"AliasChain", "namespace c = ::coro; namespace d = c; d::Loop loop;", false},
        GuardCase{"UsingType", "using coro::Loop; Loop loop;", false},
        GuardCase{"UsingNamespace", "using namespace coro; Loop loop;", false},
        GuardCase{"GlobalUsingNamespace", "using namespace ::coro; Loop loop;", false},
        GuardCase{"InternalAlias", "namespace d = coro::detail;", false},
        GuardCase{"SplitQualifier", "auto c = co_await coro\n :: detail\n :: GetContext{};", false},
        GuardCase{"DeducedWaiter", "scope.remove_waiter(node);", false},
        GuardCase{"CallbackAddress", "auto action = &coro::CancelScope::timeout_action;", false},
        GuardCase{"TaskContext", "task.bind_context(loop, scope);", false},
        GuardCase{"PromiseAlias", "using P = coro::Task<void>::promise_type;", false}),
    [](const ::testing::TestParamInfo<GuardCase>& case_info) { return case_info.param.name; });

// A normative rule states a discipline and links the owner document. Naming a
// project path or symbol forces an edit on every rename, so the fact belongs in
// an owner document under docs/.
struct RulesCase {
    const char* id;    // unique, identifier-safe
    const char* file;  // which normative rules file it lands in
    const char* source;
    bool allowed;
};

class RulesFactGuard : public ::testing::TestWithParam<RulesCase> {};

TEST_P(RulesFactGuard, Check_RuleStatesPrincipleNotProjectFact) {
    const RulesCase& test = GetParam();
    ComponentTest::TempDirectory dir{(std::filesystem::temp_directory_path() / "yaddnsc-rules-guard-XXXXXX").string()};
    std::filesystem::create_directories(dir.path() / "rules");
    std::filesystem::create_directories(dir.path() / "src/application");
    std::filesystem::create_directories(dir.path() / "include/yaddnsc/sdk");
    // The SDK header must exist before the C-ABI rule can run.
    std::ofstream(dir.path() / "include/yaddnsc/sdk/driver_abi.h") << "#include <stdint.h>\n";
    std::ofstream(dir.path() / "src/application/probe.cpp") << "\n";
    std::ofstream(dir.path() / "rules" / test.file) << test.source;
    const auto output = dir.path() / "output.txt";
    const std::string root_argument = "-DPROJECT_SOURCE_DIR=" + dir.path().string();

    const pid_t pid = ::fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        if (std::freopen(output.c_str(), "w", stdout) == nullptr || ::dup2(STDOUT_FILENO, STDERR_FILENO) == -1) {
            ::_exit(126);
        }
        ::execl(YADDNSC_TEST_CMAKE, YADDNSC_TEST_CMAKE, root_argument.c_str(), "-P", YADDNSC_TEST_GUARD, nullptr);
        ::_exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited == -1 && errno == EINTR);
    ASSERT_EQ(waited, pid);
    ASSERT_TRUE(WIFEXITED(status));
    const int exit_code = WEXITSTATUS(status);
    ASSERT_NE(exit_code, 126);
    ASSERT_NE(exit_code, 127);
    std::ifstream stream{output};
    const std::string diagnostic{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    EXPECT_EQ(exit_code == 0, test.allowed) << diagnostic;
    if (!test.allowed) {
        EXPECT_NE(diagnostic.find("rules/"), std::string::npos) << diagnostic;
    }
}

INSTANTIATE_TEST_SUITE_P(
    RulesFacts, RulesFactGuard,
    ::testing::Values(
        RulesCase{"PublicUtilPath", "02-implementation.md", "Reuse utilities from `include/yaddnsc/util/`.\n", false},
        RulesCase{"QualifiedSymbol", "02-implementation.md", "Await `coro::Task` from the application layer.\n", false},
        RulesCase{"LoggingMacro", "03-error-handling.md", "Log through the `YLOG_INFO` macro.\n", false},
        RulesCase{"BudgetConstant", "04-quality-and-process.md", "One cycle is bounded by `UPDATE_BUDGET`.\n", false},
        RulesCase{"SourcePath", "04-quality-and-process.md", "See `src/application/services.h`.\n", false},
        RulesCase{"BootstrapName", "01-language-and-build.md", "Use the CPM.cmake setup in `cmake/`.\n", false},
        RulesCase{"OwnerDocLink", "02-implementation.md", "See [Layers](../docs/architecture.md#layers).\n", true},
        RulesCase{"SlashProse", "04-quality-and-process.md", "Build/test/CI details belong in `docs/development.md`.\n",
                  true},
        RulesCase{"PlainPrinciple", "01-language-and-build.md",
                  "State the principle and link the owner document instead.\n", true},
        RulesCase{"GenericPrinciple", "03-error-handling.md", "A deadline wraps the operation; see Architecture.\n",
                  true},
        RulesCase{"ExamplesExempt", "05-examples.md",
                  "See `src/infrastructure/dns/resolver/resolver.h` for the contract.\n", true}),
    [](const ::testing::TestParamInfo<RulesCase>& case_info) { return case_info.param.id; });

}  // namespace
