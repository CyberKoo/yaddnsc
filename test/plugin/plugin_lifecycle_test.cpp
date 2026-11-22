/// Plugin lifecycle contract tests for the v1 ABI: descriptor validation, create → update → destroy ordering,
/// concurrent instances of one package, package lease vs. dlclose ordering, and the loader's rejection matrix (missing
/// file/symbols, magic, revision, descriptor struct_size) including the manual-load fail-fast vs. auto-discover skip
/// policy from the README behaviour table.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "infrastructure/plugin/async/package.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_loader.h"
#include "infrastructure/plugin/plugin_error.h"
#include "infrastructure/plugin/plugin_loader.h"
#include "infrastructure/plugin/plugin_package.h"
#include "infrastructure/plugin/shared_library.h"
#include "infrastructure/plugin/sync/driver_instance.h"
#include "infrastructure/plugin/sync/package.h"
#include "mocks/allocation_fault.h"
#include "plugin/plugin_test_doubles.h"

namespace {

constexpr std::string_view PLUGIN_PATH = TEST_PLUGIN_PATH;

using ControlSetFailures = void (*)(int);
using ControlResetState = void (*)();
using ControlGetState = void (*)(uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*, uint64_t*);

/// Resolve the test plugin's control exports (not part of the driver ABI).
/// The control library is a second dlopen of the already-loaded module, so
/// it shares the plugin's globals; it only keeps the mapping alive.
struct PluginControl {
    SharedLibrary library;
    ControlSetFailures set_failures = nullptr;
    ControlResetState reset_state = nullptr;
    ControlGetState get_state = nullptr;
};

[[nodiscard]] PluginControl resolve_control() {
    PluginControl control;
    auto library = SharedLibrary::open(std::string(PLUGIN_PATH));
    EXPECT_TRUE(library.has_value()) << library.error();
    if (!library) {
        return control;
    }
    control.library = std::move(*library);
    control.set_failures =
        reinterpret_cast<ControlSetFailures>(control.library.resolve("test_plugin_set_create_failures"));  // NOLINT
    control.reset_state =
        reinterpret_cast<ControlResetState>(control.library.resolve("test_plugin_reset_state"));              // NOLINT
    control.get_state = reinterpret_cast<ControlGetState>(control.library.resolve("test_plugin_get_state"));  // NOLINT
    return control;
}

}  // namespace

TEST(PluginLifecycle, DescriptorIsCopiedIntoHostStorage) {
    auto loaded = test::load_sync_package(std::string(PLUGIN_PATH));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    const auto& package = *loaded;
    const auto& descriptor = package->descriptor();
    EXPECT_EQ(descriptor.name, "test_driver_plugin");
    EXPECT_EQ(descriptor.version, "0.0.0");
    EXPECT_EQ(descriptor.author, "yaddnsc");
    EXPECT_EQ(descriptor.description, "Contract-test whiteboard driver");
    EXPECT_TRUE(package->capabilities().supports(domain::RecordKind::A));
    EXPECT_TRUE(package->capabilities().supports(domain::RecordKind::AAAA));
    EXPECT_FALSE(package->capabilities().supports(domain::RecordKind::TXT));
    EXPECT_EQ(descriptor.abi_major, YADDNSC_DRIVER_ABI_MAJOR);
    EXPECT_EQ(descriptor.abi_minor, UINT16_C(0));  // the whiteboard plugin declares its minimum as ABI 1.0
}

TEST(PluginLifecycle, CreateUpdateDestroyOrdering) {
    auto loaded = test::load_sync_package(std::string(PLUGIN_PATH));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    auto control = resolve_control();
    ASSERT_NE(control.reset_state, nullptr);
    ASSERT_NE(control.get_state, nullptr);

    control.reset_state();
    {
        HostUpdateContext host;
        const auto services = host.context.make_services();
        const auto result = run_sync_cycle(**loaded, services, R"({"op":"success"})");
        ASSERT_EQ(result.update_status, YADDNSC_STATUS_OK) << result.error_message;
    }

    uint64_t creates = 0, updates = 0, destroys = 0, create_seq = 0, update_seq = 0, destroy_seq = 0;
    control.get_state(&creates, &updates, &destroys, &create_seq, &update_seq, &destroy_seq);
    EXPECT_EQ(creates, 1u);
    EXPECT_EQ(updates, 1u);
    EXPECT_EQ(destroys, 1u);
    EXPECT_LT(create_seq, update_seq);
    EXPECT_LT(update_seq, destroy_seq);
}

TEST(PluginLifecycle, ConcurrentInstancesOfOneModule) {
    auto loaded = test::load_sync_package(std::string(PLUGIN_PATH));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    auto control = resolve_control();
    ASSERT_NE(control.reset_state, nullptr);
    ASSERT_NE(control.get_state, nullptr);
    control.reset_state();

    // Two instances of the same package may be updated concurrently.
    constexpr size_t THREAD_COUNT = 4;
    std::array<SyncCycleResult, THREAD_COUNT> results{};
    std::array<std::thread, THREAD_COUNT> threads{};
    for (size_t i = 0; i < THREAD_COUNT; ++i) {
        threads[i] = std::thread([&, i] {
            HostUpdateContext host;
            const auto services = host.context.make_services();
            results[i] = run_sync_cycle(**loaded, services, R"({"op":"success"})");
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    for (const auto& result : results) {
        EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
        EXPECT_EQ(result.update_status, YADDNSC_STATUS_OK) << result.error_message;
    }

    uint64_t creates = 0, updates = 0, destroys = 0, unused = 0;
    control.get_state(&creates, &updates, &destroys, &unused, &unused, &unused);
    EXPECT_EQ(creates, THREAD_COUNT);
    EXPECT_EQ(updates, THREAD_COUNT);
    EXPECT_EQ(destroys, THREAD_COUNT);
}

TEST(PluginLifecycle, InstanceLeaseKeepsModuleAliveAfterCatalogRemoval) {
    DriverCatalog catalog;
    {
        // The catalog keeps the only package reference: the test proves that
        // the instance lease alone keeps the image mapped afterwards.
        auto loaded = test::load_sync_package(std::string(PLUGIN_PATH));
        ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
        catalog.register_package(*loaded);
    }

    auto package = std::dynamic_pointer_cast<const plugin::SyncPluginPackage>(catalog.find("test_driver_plugin"));
    ASSERT_NE(package, nullptr);

    HostUpdateContext host;
    const auto services = host.context.make_services();

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_driver* handle = nullptr;
    ASSERT_EQ(package->create(services, &handle, error), YADDNSC_STATUS_OK);
    ASSERT_NE(handle, nullptr);

    // The instance holds the only remaining lease after the catalog entry is
    // removed; the plugin code must stay mapped until destroy() has run.
    DriverInstance instance(package, handle);
    ASSERT_NO_THROW(catalog.unload_driver("test_driver_plugin"));
    EXPECT_TRUE(catalog.loaded_drivers().empty());

    const auto request =
        make_update_request("192.0.2.1", "A", "example.com", "www", "www.example.com", R"({"op":"success"})");
    EXPECT_EQ(instance.update(request, error), YADDNSC_STATUS_OK)
        << std::string_view(error.message.data, error.message.size);

    // ~DriverInstance() calls destroy() while the lease is still held; the
    // final dlclose happens only after the instance is gone.
}

TEST(PluginLifecycle, DestroyRunsAfterFailedUpdate) {
    DriverCatalog catalog;
    {
        auto loaded = test::load_sync_package(std::string(PLUGIN_PATH));
        ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
        catalog.register_package(*loaded);
    }

    auto control = resolve_control();
    ASSERT_NE(control.reset_state, nullptr);
    ASSERT_NE(control.get_state, nullptr);
    control.reset_state();

    HostUpdateContext host;
    const auto services = host.context.make_services();
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    {
        auto package = std::dynamic_pointer_cast<const plugin::SyncPluginPackage>(catalog.find("test_driver_plugin"));
        ASSERT_NE(package, nullptr);
        ASSERT_EQ(package->create(services, &handle, error), YADDNSC_STATUS_OK);
        ASSERT_NE(handle, nullptr);

        // The instance holds the only remaining lease once the catalog entry
        // is gone; a failed update must not skip destroy(), and destroy()
        // must still run before the module is dlclose'd.
        DriverInstance instance(package, handle);
        package.reset();
        ASSERT_NO_THROW(catalog.unload_driver("test_driver_plugin"));
        EXPECT_TRUE(catalog.loaded_drivers().empty());

        const auto request =
            make_update_request("192.0.2.1", "A", "example.com", "www", "www.example.com",
                                R"({"op":"fail","status":"rate_limited","message":"slow down","retry_after":30})");
        EXPECT_EQ(instance.update(request, error), YADDNSC_STATUS_RATE_LIMITED)
            << std::string_view(error.message.data, error.message.size);
        EXPECT_EQ(std::string(error.message.data, error.message.size), "slow down");
        EXPECT_EQ(error.retry_after_seconds, 30u);
    }

    // ~DriverInstance() ran destroy() after the failed update while the lease
    // kept the module mapped; the counters stay readable through the control
    // mapping, which outlives the module handle itself.
    uint64_t creates = 0, updates = 0, destroys = 0, create_seq = 0, update_seq = 0, destroy_seq = 0;
    control.get_state(&creates, &updates, &destroys, &create_seq, &update_seq, &destroy_seq);
    EXPECT_EQ(creates, 1u);
    EXPECT_EQ(updates, 1u);
    EXPECT_EQ(destroys, 1u);
    EXPECT_LT(create_seq, update_seq);
    EXPECT_LT(update_seq, destroy_seq);
}

// ===========================================================================
//  Loader rejection matrix
// ===========================================================================

TEST(PluginLifecycle, LoaderRejectsMissingFile) {
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.load.emplace_back("/nonexistent/driver.so");  // absolute path is used as-is

    const auto loaded = DriverLoader::load(settings);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code, plugin::DriverLoadError::Code::LOAD_FAILED);
    EXPECT_THAT(loaded.error().message,
                ::testing::HasSubstr("Driver library 'driver.so' not found at /nonexistent/driver.so"));
}

TEST(PluginLifecycle, LoaderRejectsWrongRevision) {
    auto module = PluginLoader::load(BAD_REVISION_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::ABI_MISMATCH);
    EXPECT_THAT(module.error().message, ::testing::HasSubstr(BAD_REVISION_FIXTURE));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("reports ABI 0.0"));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("this host implements ABI 1.x"));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("use a driver built for ABI v1"));
}

TEST(PluginLifecycle, LoaderRejectsWrongMagic) {
    auto module = PluginLoader::load(BAD_MAGIC_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::ABI_MISMATCH);
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("is not a valid yaddnsc driver (magic mismatch)"));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr(BAD_MAGIC_FIXTURE));
}

TEST(PluginLifecycle, LoaderRejectsMissingEntryPoints) {
    auto module = PluginLoader::load(MISSING_SYMBOL_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::MISSING_SYMBOL);
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("does not export the required v1 entry points"));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr(MISSING_SYMBOL_FIXTURE));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("use a driver built for ABI v1"));
}

TEST(PluginLifecycle, LoaderRejectsNewerMinor) {
    auto module = PluginLoader::load(NEWER_MINOR_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::ABI_MISMATCH);
    EXPECT_THAT(module.error().message, ::testing::HasSubstr(NEWER_MINOR_FIXTURE));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("requires ABI 1.2"));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("this host provides 1.1"));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("upgrade yaddnsc"));
}

TEST(PluginLifecycle, LoaderRejectsBaselineBelowAbi10) {
    auto module = PluginLoader::load(SHORT_DESCRIPTOR_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::ABI_MISMATCH);
    EXPECT_THAT(module.error().message, ::testing::HasSubstr(SHORT_DESCRIPTOR_FIXTURE));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("is below the ABI 1.0 baseline"));
}

TEST(PluginLifecycle, LoaderRejectsTruncatedDescriptor) {
    auto module = PluginLoader::load(SMALL_DESCRIPTOR_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::ABI_MISMATCH);
    EXPECT_THAT(module.error().message,
                ::testing::HasSubstr("descriptor struct_size 4 does not cover the ABI version prefix"));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr(SMALL_DESCRIPTOR_FIXTURE));
}

TEST(PluginLifecycle, LoaderRejectsInvalidDescriptorViewsAndCapabilities) {
    const std::array fixtures{
        std::pair{std::string_view{INVALID_NAME_FIXTURE}, "name"},
        std::pair{std::string_view{INVALID_VERSION_FIXTURE}, "version"},
        std::pair{std::string_view{INVALID_AUTHOR_FIXTURE}, "author"},
        std::pair{std::string_view{INVALID_DESCRIPTION_FIXTURE}, "description"},
        std::pair{std::string_view{INVALID_CAPABILITIES_FIXTURE}, "capabilities"},
    };

    for (const auto& [path, field] : fixtures) {
        auto module = PluginLoader::load(std::string(path));
        ASSERT_FALSE(module.has_value()) << path;
        EXPECT_EQ(module.error().code, plugin::PluginError::Code::CONTRACT_VIOLATION) << path;
        EXPECT_THAT(module.error().message, ::testing::HasSubstr(path)) << path;
        EXPECT_THAT(module.error().message, ::testing::HasSubstr(field)) << path;
    }
}

// ===========================================================================
//  Descriptor exception firewall — a plugin whose get_descriptor() throws
//  across the C ABI. The loader is the boundary that contains the escape: it
//  must report a contract violation and let no exception reach the caller,
//  not even std::bad_alloc and not a second exception raised while the
//  detailed diagnostic is being formatted.
// ===========================================================================

TEST(PluginLifecycle, LoaderContainsDescriptorRuntimeError) {
    // A standard exception keeps its detail: the path and the what() text.
    const auto module = PluginLoader::load(THROWING_DESCRIPTOR_RUNTIME_ERROR_FIXTURE);

    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::CONTRACT_VIOLATION);
    const std::string diagnostic(module.error().diagnostic());
    EXPECT_THAT(diagnostic, ::testing::HasSubstr("get_descriptor() threw"));
    EXPECT_THAT(diagnostic, ::testing::HasSubstr("descriptor exploded"));
    EXPECT_THAT(diagnostic, ::testing::HasSubstr(THROWING_DESCRIPTOR_RUNTIME_ERROR_FIXTURE));
}

TEST(PluginLifecycle, LoaderContainsNonStandardDescriptorException) {
    // An exception outside the std::exception hierarchy has no what() text;
    // the loader still reports where the escape happened.
    const auto module = PluginLoader::load(THROWING_DESCRIPTOR_NON_STD_FIXTURE);

    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::CONTRACT_VIOLATION);
    const std::string diagnostic(module.error().diagnostic());
    EXPECT_THAT(diagnostic, ::testing::HasSubstr("unknown exception"));
    EXPECT_THAT(diagnostic, ::testing::HasSubstr(THROWING_DESCRIPTOR_NON_STD_FIXTURE));
}

TEST(PluginLifecycle, LoaderContainsDescriptorBadAllocWithoutFormatting) {
    // An out-of-memory exception is answered with the static fallback: no
    // detailed text is built, because building one would need the very
    // resource the plugin exhausted.
    const auto module = PluginLoader::load(THROWING_DESCRIPTOR_BAD_ALLOC_FIXTURE);

    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, plugin::PluginError::Code::CONTRACT_VIOLATION);
    EXPECT_TRUE(module.error().message.empty());
    EXPECT_FALSE(module.error().diagnostic().empty());
}

namespace {

/// Allocation-fault injection state for the loader's diagnostic firewall.
///
/// The replaced global allocator calls the armed hook for every allocation;
/// the hook fails the g_fail_at-th allocation of the armed window.
std::size_t g_allocations_seen = 0;
std::size_t g_fail_at = 0;

void fail_selected_allocation(std::size_t /*size*/) {
    ++g_allocations_seen;
    if (g_fail_at != 0 && g_allocations_seen == g_fail_at) {
        throw std::bad_alloc{};
    }
}

}  // namespace

TEST(PluginLifecycle, DescriptorDiagnosticFallsBackWhenFormattingFails) {
    // A failure while the detailed diagnostic is formatted must not become a
    // second exception crossing the loader boundary: the static fallback
    // answers it.
    //
    // The non-standard fixture is used on purpose: it throws without
    // allocating, so an injected allocation fault can only be absorbed by the
    // host's diagnostic formatting. The allocation index is swept because it
    // is an implementation detail of the formatter.
    constexpr std::size_t MAX_INJECTED_ALLOCATIONS = 64;

    // Baseline: without an injected fault the detailed diagnostic is built.
    {
        const auto module = PluginLoader::load(THROWING_DESCRIPTOR_NON_STD_FIXTURE);
        ASSERT_FALSE(module.has_value());
        EXPECT_FALSE(module.error().message.empty());
    }

    bool saw_fallback = false;
    for (std::size_t fail_at = 1; fail_at <= MAX_INJECTED_ALLOCATIONS && !saw_fallback; ++fail_at) {
        std::expected<std::shared_ptr<const plugin::PluginPackage>, plugin::PluginError> module;
        g_fail_at = fail_at;
        g_allocations_seen = 0;
        test::set_allocation_fault_hook(&fail_selected_allocation);
        try {
            module = PluginLoader::load(THROWING_DESCRIPTOR_NON_STD_FIXTURE);
        } catch (const std::bad_alloc&) {
            // The injected fault hit a host-side allocation outside the
            // plugin's exception handling. That is a host defect, not the
            // loader's contract; the next index targets the diagnostic.
        }
        test::set_allocation_fault_hook(nullptr);
        g_fail_at = 0;

        if (!module) {
            std::optional<plugin::PluginError> error = std::move(module.error());
            saw_fallback = error->message.empty() && !error->diagnostic().empty();
        }
    }
    EXPECT_TRUE(saw_fallback) << "no injected allocation failure reached the diagnostic formatting";
}

// ===========================================================================
//  Path validation — an embedded NUL byte must be rejected before the path
//  reaches dlopen(), which takes a C string and would truncate it there.
// ===========================================================================

TEST(PluginLifecycle, SharedLibraryRejectsPathWithEmbeddedNul) {
    // Without the rejection the path would be truncated at the NUL and a
    // different image — the real plugin — would be loaded.
    std::string path{PLUGIN_PATH};
    path.push_back('\0');
    path += "ignored";

    const auto library = SharedLibrary::open(path);

    ASSERT_FALSE(library.has_value());
    EXPECT_THAT(library.error(), ::testing::HasSubstr("embedded NUL byte"));
}

// ===========================================================================
//  Optional validate entry (optional since ABI 1.0)
// ===========================================================================

TEST(PluginLifecycle, LoaderAcceptsPluginWithoutOptionalValidateEntry) {
    // A complete plugin built against an SDK predating yaddnsc_driver_validate
    // must load exactly like before — the missing entry is not an error.
    auto module = PluginLoader::load(NO_VALIDATE_FIXTURE);
    ASSERT_TRUE(module.has_value()) << module.error().message;
    EXPECT_EQ((*module)->descriptor().name, "no_validate");
    EXPECT_FALSE((*module)->supports_validate());
}

TEST(PluginLifecycle, LoaderValidateFailsClosedWhenEntryIsMissing) {
    auto loaded = test::load_sync_package(NO_VALIDATE_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // The trampoline fails closed without calling a plugin: without the entry
    // the host cannot confirm driver_params, so the absence is a validation
    // failure, never a silent pass.
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const yaddnsc_string param{R"({"anything":true})", 16};
    EXPECT_EQ((*loaded)->validate(nullptr, param, error), YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_THAT(std::string(error.message.data, error.message.size),
                ::testing::HasSubstr("does not export yaddnsc_driver_validate"));
}

TEST(PluginLifecycle, ValidateEntryAvailableOnCurrentSdkPlugin) {
    auto module = PluginLoader::load(std::string(PLUGIN_PATH));
    ASSERT_TRUE(module.has_value()) << module.error().message;
    EXPECT_TRUE((*module)->supports_validate());
}

// ===========================================================================
//  Entry exception firewall — a misbehaving plugin throwing across the C ABI.
//  The ABI forbids exceptions; each trampoline must translate an escape into
//  YADDNSC_STATUS_INTERNAL_ERROR with a usable message instead of unwinding
//  into the host's frame.
// ===========================================================================

TEST(PluginLifecycle, EntryFirewallTranslatesCreateException) {
    auto loaded = test::load_sync_package(THROWING_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    HostUpdateContext host;
    const auto services = host.context.make_services();
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_driver* handle = nullptr;
    EXPECT_EQ((*loaded)->create(services, &handle, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "create exploded");
}

TEST(PluginLifecycle, EntryFirewallTranslatesUpdateException) {
    auto loaded = test::load_sync_package(THROWING_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // create throws, so no real handle exists; the fixture's update throws
    // before touching the handle and the firewall must still translate it.
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    int token = 0;
    auto* handle = reinterpret_cast<yaddnsc_driver*>(&token);  // NOLINT
    const auto request = make_update_request("192.0.2.1", "A", "example.com", "www", "www.example.com", "{}");
    EXPECT_EQ((*loaded)->update(handle, request, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "update exploded");
}

TEST(PluginLifecycle, EntryFirewallTranslatesValidateException) {
    auto loaded = test::load_sync_package(THROWING_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // validate throws a non-std type → the catch-all arm reports the generic
    // wording rather than the exception's (nonexistent) what().
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    int token = 0;
    auto* handle = reinterpret_cast<yaddnsc_driver*>(&token);  // NOLINT
    const yaddnsc_string param{"{}", 2};
    EXPECT_EQ((*loaded)->validate(handle, param, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "unknown exception from plugin validate");
}

TEST(PluginLifecycle, DestroyFirewallContainsPluginException) {
    auto loaded = test::load_sync_package(THROWING_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    int token = 0;
    auto* handle = reinterpret_cast<yaddnsc_driver*>(&token);  // NOLINT
    EXPECT_NO_THROW((*loaded)->destroy(handle));
}

TEST(PluginLifecycle, AsyncDestroyFailureIsFatal) {
    auto loaded = test::load_async_package(ASYNC_THROWING_DESTROY_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    yaddnsc_async_services services{};
    services.struct_size = static_cast<uint32_t>(sizeof(services));
    services.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    services.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_async_driver* handle = nullptr;
    ASSERT_EQ((*loaded)->async_create(services, &handle, error), YADDNSC_STATUS_OK);
    ASSERT_NE(handle, nullptr);

    EXPECT_DEATH((*loaded)->async_destroy(handle), "");
}

TEST(PluginLifecycle, DestroyFirewallContainsNonStdException) {
    auto loaded = test::load_sync_package(THROWING_DESTROY_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // create/update succeed so the cycle reaches destroy; destroy throws a
    // non-std type (int) — the catch-all firewall arm must swallow the
    // escape without unwinding into the host or terminating the process.
    HostUpdateContext host;
    const auto services = host.context.make_services();
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver* handle = nullptr;
    ASSERT_EQ((*loaded)->create(services, &handle, error), YADDNSC_STATUS_OK);
    ASSERT_NE(handle, nullptr);

    const auto request = make_update_request("192.0.2.1", "A", "example.com", "www", "www.example.com", "{}");
    EXPECT_EQ((*loaded)->update(handle, request, error), YADDNSC_STATUS_OK)
        << std::string_view(error.message.data, error.message.size);

    EXPECT_NO_THROW((*loaded)->destroy(handle));
}

// ===========================================================================
//  Create handle-ownership backstop — a plugin that stores a handle and
//  then reports failure (or throws) violates the ABI contract; the host
//  must destroy and clear the leaked handle before returning the failure,
//  because no DriverInstance exists to own the destroy() call.
// ===========================================================================

TEST(PluginLifecycle, CreateFailureAfterStoringHandleDestroysAndClearsIt) {
    auto loaded = test::load_sync_package(LEAKY_CREATE_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;

    // Second dlopen of the same module: shares the fixture's globals and
    // keeps the mapping alive for the control exports.
    auto control_library = SharedLibrary::open(LEAKY_CREATE_FIXTURE);
    ASSERT_TRUE(control_library.has_value()) << control_library.error();
    using SetMode = void (*)(int);
    using GetState = void (*)(uint64_t*, uintptr_t*);
    using Token = uintptr_t (*)();
    const auto set_mode = reinterpret_cast<SetMode>(control_library->resolve("leaky_create_set_mode"));     // NOLINT
    const auto get_state = reinterpret_cast<GetState>(control_library->resolve("leaky_create_get_state"));  // NOLINT
    const auto token = reinterpret_cast<Token>(control_library->resolve("leaky_create_token"));             // NOLINT
    ASSERT_NE(set_mode, nullptr);
    ASSERT_NE(get_state, nullptr);
    ASSERT_NE(token, nullptr);

    HostUpdateContext host;
    const auto services = host.context.make_services();

    // Status mode: create stores a handle, then returns a failure status.
    set_mode(0);
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_driver* handle = nullptr;
    EXPECT_EQ((*loaded)->create(services, &handle, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "create failed after storing a handle");

    uint64_t destroys = 0;
    uintptr_t last_destroyed = 0;
    get_state(&destroys, &last_destroyed);
    EXPECT_EQ(destroys, 1u);
    EXPECT_EQ(last_destroyed, token());

    // Exception mode: create stores a handle, then throws across the ABI —
    // the firewall reports the exception and the same backstop still runs.
    set_mode(1);
    error = {};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    handle = nullptr;
    EXPECT_EQ((*loaded)->create(services, &handle, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "create exploded after storing a handle");

    get_state(&destroys, &last_destroyed);
    EXPECT_EQ(destroys, 2u);
    EXPECT_EQ(last_destroyed, token());

    // Instance-owned mode: the error bytes die with the failed create handle.
    set_mode(2);
    error = {};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    handle = nullptr;
    EXPECT_EQ((*loaded)->create(services, &handle, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "create failed after storing a handle");

    get_state(&destroys, &last_destroyed);
    EXPECT_EQ(destroys, 3u);
    EXPECT_EQ(last_destroyed, token());
}

namespace {

class AsyncCreateCleanupTest : public testing::Test {
protected:
    void SetUp() override {
        auto loaded = test::load_async_package(ASYNC_CREATE_CLEANUP_FIXTURE);
        ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
        package = *loaded;
        auto library = SharedLibrary::open(ASYNC_CREATE_CLEANUP_FIXTURE);
        ASSERT_TRUE(library.has_value()) << library.error();
        control = std::move(*library);
        set_scenario =
            reinterpret_cast<void (*)(int)>(control->resolve("async_create_cleanup_set_scenario"));  // NOLINT
        get_state = reinterpret_cast<void (*)(uint64_t*, uint64_t*, uintptr_t*)>(
            control->resolve("async_create_cleanup_get_state"));  // NOLINT
        ASSERT_NE(set_scenario, nullptr);
        ASSERT_NE(get_state, nullptr);
        services.struct_size = static_cast<uint32_t>(sizeof(services));
        services.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
        services.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
        previous_logger = spdlog::default_logger();
    }

    void TearDown() override {
        if (previous_logger) {
            spdlog::set_default_logger(previous_logger);
        }
    }

    std::optional<std::shared_ptr<const plugin::AsyncPluginPackage>> package;
    std::optional<SharedLibrary> control;
    void (*set_scenario)(int) = nullptr;
    void (*get_state)(uint64_t*, uint64_t*, uintptr_t*) = nullptr;
    yaddnsc_async_services services{};
    std::shared_ptr<spdlog::logger> previous_logger;
};

class ThrowingCleanupLogSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    unsigned calls = 0;

protected:
    void sink_it_(const spdlog::details::log_msg&) override {
        ++calls;
        throw std::runtime_error("cleanup diagnostic failed");
    }

    void flush_() override {}
};

}  // namespace

TEST_F(AsyncCreateCleanupTest, CapturesValidReportsBeforeDestroyAndPreservesInvalidReports) {
    for (int scenario = 0; scenario <= 13; ++scenario) {
        if (scenario == 10) {
            continue;  // Throwing destroy is covered by the death test below.
        }
        SCOPED_TRACE(scenario);
        set_scenario(scenario);
        yaddnsc_error error{};
        error.struct_size = scenario == 11 ? YADDNSC_ERROR_MIN_SIZE - 1 : static_cast<uint32_t>(sizeof(error));
        int caller_token = 0;
        auto* handle = reinterpret_cast<yaddnsc_async_driver*>(&caller_token);  // NOLINT
        const auto status = (*package)->async_create(services, &handle, error);
        EXPECT_EQ(handle, nullptr);
        uint64_t creates = 0;
        uint64_t destroys = 0;
        uintptr_t plugin_message = 0;
        get_state(&creates, &destroys, &plugin_message);
        EXPECT_EQ(creates, 1u);
        EXPECT_EQ(destroys, 1u);
        EXPECT_NE(reinterpret_cast<uintptr_t>(error.message.data), plugin_message);

        if (scenario == 0 || scenario == 1 || scenario == 2 || scenario == 7 || scenario == 8) {
            const bool threw = scenario == 7 || scenario == 8;
            EXPECT_EQ(status, threw ? YADDNSC_STATUS_INTERNAL_ERROR : YADDNSC_STATUS_RATE_LIMITED);
            ASSERT_GE(error.struct_size, YADDNSC_ERROR_MIN_SIZE);
            EXPECT_EQ(error.status, status);
            EXPECT_EQ(error.retry_after_seconds, threw ? 0u : 73u);
            ASSERT_NE(error.message.data, nullptr);
            const std::string_view message(error.message.data, error.message.size);
            if (scenario == 7) {
                EXPECT_EQ(message, "async create threw after handle");
            } else if (scenario == 8) {
                EXPECT_EQ(message, "unknown exception from plugin async_create");
            } else {
                EXPECT_EQ(message, std::string(scenario == 1 ? 0 : scenario == 2 ? 511 : 23, 'm'));
            }
        } else {
            EXPECT_EQ(status, scenario == 6    ? static_cast<yaddnsc_status>(9999)
                              : scenario == 12 ? YADDNSC_STATUS_PENDING
                                               : YADDNSC_STATUS_RATE_LIMITED);
            // This is the existing gateway's malformed-report predicate: it
            // must not become a normal, valid RATE_LIMITED error after cleanup.
            EXPECT_LT(error.struct_size, YADDNSC_ERROR_MIN_SIZE);
            EXPECT_EQ(error.message.data, nullptr);
            EXPECT_EQ(error.retry_after_seconds, 0u);
        }
    }
}

TEST_F(AsyncCreateCleanupTest, DiagnosticExceptionsCannotSkipCleanupOrReplaceSnapshot) {
    auto sink = std::make_shared<ThrowingCleanupLogSink>();
    auto logger = std::make_shared<spdlog::logger>("throwing-cleanup-log", sink);
    logger->set_level(spdlog::level::warn);
    logger->set_error_handler([](const std::string&) { throw std::runtime_error("log handler failed"); });
    spdlog::set_default_logger(logger);
    set_scenario(0);
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_async_driver* handle = nullptr;
    EXPECT_EQ((*package)->async_create(services, &handle, error), YADDNSC_STATUS_RATE_LIMITED);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(std::string_view(error.message.data, error.message.size), std::string(23, 'm'));
    EXPECT_EQ(error.retry_after_seconds, 73u);
    EXPECT_EQ(sink->calls, 1u);
    uint64_t creates = 0;
    uint64_t destroys = 0;
    uintptr_t message = 0;
    get_state(&creates, &destroys, &message);
    EXPECT_EQ(creates, 1u);
    EXPECT_EQ(destroys, 1u);
}

TEST_F(AsyncCreateCleanupTest, FailedCreateDestroyExceptionIsFatal) {
    set_scenario(10);
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_async_driver* handle = nullptr;
    EXPECT_DEATH({ [[maybe_unused]] const auto status = (*package)->async_create(services, &handle, error); }, "");
}

TEST(PluginLifecycle, CreateNullOutDriverIsInvalidArgument) {
    auto loaded = test::load_sync_package(std::string(PLUGIN_PATH));
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    const auto control = resolve_control();
    ASSERT_NE(control.reset_state, nullptr);
    ASSERT_NE(control.get_state, nullptr);
    control.reset_state();

    HostUpdateContext host;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    EXPECT_EQ((*loaded)->create(host.services(), nullptr, error), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(std::string_view(error.message.data, error.message.size), "out_driver must not be null");

    uint64_t creates = 0;
    uint64_t updates = 0;
    uint64_t destroys = 0;
    uint64_t create_seq = 0;
    uint64_t update_seq = 0;
    uint64_t destroy_seq = 0;
    control.get_state(&creates, &updates, &destroys, &create_seq, &update_seq, &destroy_seq);
    EXPECT_EQ(creates, 0u);
}

TEST(PluginLifecycle, CreateOkWithNullHandleIsInternalError) {
    auto loaded = test::load_sync_package(CREATE_CONTRACT_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    auto control_library = SharedLibrary::open(CREATE_CONTRACT_FIXTURE);
    ASSERT_TRUE(control_library.has_value()) << control_library.error();
    using SetMode = void (*)(int);
    using GetState = void (*)(uint64_t*, uintptr_t*);
    const auto set_mode = reinterpret_cast<SetMode>(control_library->resolve("create_contract_set_mode"));     // NOLINT
    const auto get_state = reinterpret_cast<GetState>(control_library->resolve("create_contract_get_state"));  // NOLINT
    ASSERT_NE(set_mode, nullptr);
    ASSERT_NE(get_state, nullptr);

    set_mode(0);
    HostUpdateContext host;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    int sentinel = 0;
    auto* handle = reinterpret_cast<yaddnsc_driver*>(&sentinel);  // NOLINT
    EXPECT_EQ((*loaded)->create(host.services(), &handle, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string_view(error.message.data, error.message.size), "create returned OK with a null handle");

    uint64_t destroys = 0;
    uintptr_t last_destroyed = 0;
    get_state(&destroys, &last_destroyed);
    EXPECT_EQ(destroys, 0u);
}

TEST(PluginLifecycle, CreateFailureWithoutHandleLeavesNullAndDoesNotDestroyCallerPointer) {
    auto loaded = test::load_sync_package(CREATE_CONTRACT_FIXTURE);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    auto control_library = SharedLibrary::open(CREATE_CONTRACT_FIXTURE);
    ASSERT_TRUE(control_library.has_value()) << control_library.error();
    using SetMode = void (*)(int);
    using GetState = void (*)(uint64_t*, uintptr_t*);
    const auto set_mode = reinterpret_cast<SetMode>(control_library->resolve("create_contract_set_mode"));     // NOLINT
    const auto get_state = reinterpret_cast<GetState>(control_library->resolve("create_contract_get_state"));  // NOLINT
    ASSERT_NE(set_mode, nullptr);
    ASSERT_NE(get_state, nullptr);

    set_mode(1);
    HostUpdateContext host;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    int sentinel = 0;
    auto* handle = reinterpret_cast<yaddnsc_driver*>(&sentinel);  // NOLINT
    EXPECT_EQ((*loaded)->create(host.services(), &handle, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string_view(error.message.data, error.message.size), "create failed without a handle");

    uint64_t destroys = 0;
    uintptr_t last_destroyed = 0;
    get_state(&destroys, &last_destroyed);
    EXPECT_EQ(destroys, 0u);
    EXPECT_EQ(last_destroyed, 0u);
}

TEST(PluginLifecycle, ManualLoadFailsFastOnAbiMismatch) {
    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.load.emplace_back(BAD_REVISION_FIXTURE);  // absolute path is used as-is

    // A manual load failure aborts the whole load: the loader reports the
    // error instead of a report with the mismatched library skipped.
    const auto loaded = DriverLoader::load(settings);
    ASSERT_FALSE(loaded.has_value());
    EXPECT_EQ(loaded.error().code, plugin::DriverLoadError::Code::ABI_MISMATCH);
    EXPECT_TRUE(catalog.loaded_drivers().empty());
}

TEST(PluginLifecycle, AutoDiscoverSkipsAbiMismatchedLibraries) {
    // One directory containing only the revision-mismatched fixture: auto
    // discovery must skip it with a warning instead of aborting startup.
    char dir_template[] = "/tmp/yaddnsc_plugin_test_XXXXXX";
    auto* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(BAD_REVISION_FIXTURE, std::string(dir) + "/bad_revision.so");

    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;

    const auto loaded = DriverLoader::load(settings);
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    EXPECT_TRUE(loaded->packages.empty());
    ASSERT_EQ(loaded->skipped.size(), 1u);
    EXPECT_EQ(loaded->skipped.front().code, plugin::DriverLoadError::Code::ABI_MISMATCH);

    DriverCatalog catalog{loaded->packages};
    EXPECT_TRUE(catalog.loaded_drivers().empty());

    std::filesystem::remove_all(dir);
}
