//
// Created by Kotarou on 2026/9/17.
//

/// Plugin lifecycle contract tests for the v1 alpha ABI: descriptor validation, create → update → destroy ordering,
/// concurrent instances of one module, module lease vs. dlclose ordering, and the loader's rejection matrix (missing
/// file/symbols, magic, revision, descriptor struct_size) including the manual-load fail-fast vs. auto-discover skip
/// policy from the README behaviour table.

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <expected>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stdlib.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "infrastructure/plugin/driver_catalog.h"
#include "infrastructure/plugin/driver_instance.h"
#include "infrastructure/plugin/driver_loader.h"
#include "infrastructure/plugin/host_services.h"
#include "infrastructure/plugin/plugin_load_exception.h"
#include "infrastructure/plugin/plugin_loader.h"
#include "infrastructure/plugin/shared_library.h"
#include "plugin/plugin_test_doubles.h"

namespace {

constexpr std::string_view kPluginPath = TEST_PLUGIN_PATH;

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
    auto library = SharedLibrary::open(std::string(kPluginPath));
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
    auto module = PluginModule::load(std::string(kPluginPath));
    ASSERT_TRUE(module.has_value()) << module.error().message;

    const auto& descriptor = module->descriptor();
    EXPECT_EQ(descriptor.name, "test_driver_plugin");
    EXPECT_EQ(descriptor.version, "0.0.0");
    EXPECT_EQ(descriptor.author, "yaddnsc");
    EXPECT_EQ(descriptor.description, "Contract-test whiteboard driver");
    EXPECT_EQ(descriptor.capabilities, YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA);
    EXPECT_EQ(descriptor.api_revision, YADDNSC_DRIVER_API_REVISION);
}

TEST(PluginLifecycle, CreateUpdateDestroyOrdering) {
    auto module = PluginModule::load(std::string(kPluginPath));
    ASSERT_TRUE(module.has_value()) << module.error().message;
    auto control = resolve_control();
    ASSERT_NE(control.reset_state, nullptr);
    ASSERT_NE(control.get_state, nullptr);

    control.reset_state();
    {
        HostUpdateContext host;
        const auto services = host.context.make_services();
        const auto result = run_module_cycle(*module, services, R"({"op":"success"})");
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
    auto module = PluginModule::load(std::string(kPluginPath));
    ASSERT_TRUE(module.has_value()) << module.error().message;
    auto control = resolve_control();
    ASSERT_NE(control.reset_state, nullptr);
    ASSERT_NE(control.get_state, nullptr);
    control.reset_state();

    // Two instances of the same module may be updated concurrently.
    constexpr size_t kThreads = 4;
    std::array<ModuleCycleResult, kThreads> results{};
    std::array<std::thread, kThreads> threads{};
    for (size_t i = 0; i < kThreads; ++i) {
        threads[i] = std::thread([&, i] {
            HostUpdateContext host;
            const auto services = host.context.make_services();
            results[i] = run_module_cycle(*module, services, R"({"op":"success"})");
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
    EXPECT_EQ(creates, kThreads);
    EXPECT_EQ(updates, kThreads);
    EXPECT_EQ(destroys, kThreads);
}

TEST(PluginLifecycle, InstanceLeaseKeepsModuleAliveAfterCatalogRemoval) {
    DriverCatalog catalog;
    ASSERT_NO_THROW(catalog.load_driver(std::string(kPluginPath)));

    auto module = catalog.find("test_driver_plugin");
    ASSERT_NE(module, nullptr);

    HostUpdateContext host;
    const auto services = host.context.make_services();

    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_driver* handle = nullptr;
    ASSERT_EQ(module->create(services, &handle, error), YADDNSC_STATUS_OK);
    ASSERT_NE(handle, nullptr);

    // The instance holds the only remaining lease after the catalog entry is
    // removed; the plugin code must stay mapped until destroy() has run.
    DriverInstance instance(module, handle);
    ASSERT_NO_THROW(catalog.unload_driver("test_driver_plugin"));
    EXPECT_TRUE(catalog.get_loaded_drivers().empty());

    const auto request =
        make_update_request("192.0.2.1", "A", "example.com", "www", "www.example.com", R"({"op":"success"})");
    EXPECT_EQ(instance.update(request, error), YADDNSC_STATUS_OK)
        << std::string_view(error.message.data, error.message.size);

    // ~DriverInstance() calls destroy() while the lease is still held; the
    // final dlclose happens only after the instance is gone.
}

TEST(PluginLifecycle, DestroyRunsAfterFailedUpdate) {
    DriverCatalog catalog;
    ASSERT_NO_THROW(catalog.load_driver(std::string(kPluginPath)));

    auto control = resolve_control();
    ASSERT_NE(control.reset_state, nullptr);
    ASSERT_NE(control.get_state, nullptr);
    control.reset_state();

    HostUpdateContext host;
    const auto services = host.context.make_services();
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver *handle = nullptr;
    {
        auto module = catalog.find("test_driver_plugin");
        ASSERT_NE(module, nullptr);
        ASSERT_EQ(module->create(services, &handle, error), YADDNSC_STATUS_OK);
        ASSERT_NE(handle, nullptr);

        // The instance holds the only remaining lease once the catalog entry
        // is gone; a failed update must not skip destroy(), and destroy()
        // must still run before the module is dlclose'd.
        DriverInstance instance(module, handle);
        module.reset();
        ASSERT_NO_THROW(catalog.unload_driver("test_driver_plugin"));
        EXPECT_TRUE(catalog.get_loaded_drivers().empty());

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
    DriverCatalog catalog;
    try {
        catalog.load_driver("/nonexistent/driver.so");
        FAIL() << "expected PluginLoadException";
    } catch (const PluginLoadException& e) {
        EXPECT_THAT(std::string(e.what()),
                    ::testing::HasSubstr("Driver library 'driver.so' not found at /nonexistent/driver.so"));
    }
}

TEST(PluginLifecycle, LoaderRejectsWrongRevision) {
    auto module = PluginModule::load(BAD_REVISION_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, domain::PluginError::Code::ABI_MISMATCH);
    EXPECT_THAT(module.error().message, ::testing::HasSubstr(BAD_REVISION_FIXTURE));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("api_revision 0"));
    EXPECT_THAT(module.error().message,
                ::testing::HasSubstr("host requires " + std::to_string(YADDNSC_DRIVER_API_REVISION)));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("rebuild the driver with the current SDK"));
}

TEST(PluginLifecycle, LoaderRejectsWrongMagic) {
    auto module = PluginModule::load(BAD_MAGIC_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, domain::PluginError::Code::ABI_MISMATCH);
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("is not a valid yaddnsc driver (magic mismatch)"));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr(BAD_MAGIC_FIXTURE));
}

TEST(PluginLifecycle, LoaderRejectsMissingEntryPoints) {
    auto module = PluginModule::load(MISSING_SYMBOL_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, domain::PluginError::Code::MISSING_SYMBOL);
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("does not export the required v1 alpha entry points"));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr(MISSING_SYMBOL_FIXTURE));
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("rebuild the driver with the current SDK"));
}

TEST(PluginLifecycle, LoaderRejectsTruncatedDescriptor) {
    auto module = PluginModule::load(SMALL_DESCRIPTOR_FIXTURE);
    ASSERT_FALSE(module.has_value());
    EXPECT_EQ(module.error().code, domain::PluginError::Code::ABI_MISMATCH);
    EXPECT_THAT(module.error().message, ::testing::HasSubstr("descriptor struct_size 4 is below the required minimum"));
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
        auto module = PluginModule::load(std::string(path));
        ASSERT_FALSE(module.has_value()) << path;
        EXPECT_EQ(module.error().code, domain::PluginError::Code::CONTRACT_VIOLATION) << path;
        EXPECT_THAT(module.error().message, ::testing::HasSubstr(path)) << path;
        EXPECT_THAT(module.error().message, ::testing::HasSubstr(field)) << path;
    }
}

// ===========================================================================
//  Optional validate entry (added within api_revision 1)
// ===========================================================================

TEST(PluginLifecycle, LoaderAcceptsPluginWithoutOptionalValidateEntry) {
    // A complete plugin built against an SDK predating yaddnsc_driver_validate
    // must load exactly like before — the missing entry is not an error.
    auto module = PluginModule::load(NO_VALIDATE_FIXTURE);
    ASSERT_TRUE(module.has_value()) << module.error().message;
    EXPECT_EQ(module->descriptor().name, "no_validate");
    EXPECT_FALSE(module->supports_validate());
}

TEST(PluginLifecycle, ValidateIsSkippedWhenEntryIsMissing) {
    auto module = PluginModule::load(NO_VALIDATE_FIXTURE);
    ASSERT_TRUE(module.has_value()) << module.error().message;

    // The trampoline reports OK without touching the instance: config
    // validation skips the driver-side check instead of failing.
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const yaddnsc_string param{R"({"anything":true})", 16};
    EXPECT_EQ(module->validate(nullptr, param, error), YADDNSC_STATUS_OK);
}

TEST(PluginLifecycle, ValidateEntryAvailableOnCurrentSdkPlugin) {
    auto module = PluginModule::load(std::string(kPluginPath));
    ASSERT_TRUE(module.has_value()) << module.error().message;
    EXPECT_TRUE(module->supports_validate());
}

// ===========================================================================
//  Entry exception firewall — a misbehaving plugin throwing across the C ABI.
//  The ABI forbids exceptions; each trampoline must translate an escape into
//  YADDNSC_STATUS_INTERNAL_ERROR with a usable message instead of unwinding
//  into the host's frame.
// ===========================================================================

TEST(PluginLifecycle, EntryFirewallTranslatesCreateException) {
    auto module = PluginModule::load(THROWING_FIXTURE);
    ASSERT_TRUE(module.has_value()) << module.error().message;

    HostUpdateContext host;
    const auto services = host.context.make_services();
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    yaddnsc_driver* handle = nullptr;
    EXPECT_EQ(module->create(services, &handle, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "create exploded");
}

TEST(PluginLifecycle, EntryFirewallTranslatesUpdateException) {
    auto module = PluginModule::load(THROWING_FIXTURE);
    ASSERT_TRUE(module.has_value()) << module.error().message;

    // create throws, so no real handle exists; the fixture's update throws
    // before touching the handle and the firewall must still translate it.
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    int token = 0;
    auto* handle = reinterpret_cast<yaddnsc_driver*>(&token);  // NOLINT
    const auto request = make_update_request("192.0.2.1", "A", "example.com", "www", "www.example.com", "{}");
    EXPECT_EQ(module->update(handle, request, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "update exploded");
}

TEST(PluginLifecycle, EntryFirewallTranslatesValidateException) {
    auto module = PluginModule::load(THROWING_FIXTURE);
    ASSERT_TRUE(module.has_value()) << module.error().message;

    // validate throws a non-std type → the catch-all arm reports the generic
    // wording rather than the exception's (nonexistent) what().
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    int token = 0;
    auto* handle = reinterpret_cast<yaddnsc_driver*>(&token);  // NOLINT
    const yaddnsc_string param{"{}", 2};
    EXPECT_EQ(module->validate(handle, param, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "unknown exception from plugin validate");
}

TEST(PluginLifecycle, DestroyFirewallContainsPluginException) {
    auto module = PluginModule::load(THROWING_FIXTURE);
    ASSERT_TRUE(module.has_value()) << module.error().message;

    int token = 0;
    auto* handle = reinterpret_cast<yaddnsc_driver*>(&token);  // NOLINT
    EXPECT_NO_THROW(module->destroy(handle));
}

TEST(PluginLifecycle, DestroyFirewallContainsNonStdException) {
    auto module = PluginModule::load(THROWING_DESTROY_FIXTURE);
    ASSERT_TRUE(module.has_value()) << module.error().message;

    // create/update succeed so the cycle reaches destroy; destroy throws a
    // non-std type (int) — the catch-all firewall arm must swallow the
    // escape without unwinding into the host or terminating the process.
    HostUpdateContext host;
    const auto services = host.context.make_services();
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    yaddnsc_driver *handle = nullptr;
    ASSERT_EQ(module->create(services, &handle, error), YADDNSC_STATUS_OK);
    ASSERT_NE(handle, nullptr);

    const auto request = make_update_request("192.0.2.1", "A", "example.com", "www", "www.example.com", "{}");
    EXPECT_EQ(module->update(handle, request, error), YADDNSC_STATUS_OK)
            << std::string_view(error.message.data, error.message.size);

    EXPECT_NO_THROW(module->destroy(handle));
}

// ===========================================================================
//  Create handle-ownership backstop — a plugin that stores a handle and
//  then reports failure (or throws) violates the ABI contract; the host
//  must destroy and clear the leaked handle before returning the failure,
//  because no DriverInstance exists to own the destroy() call.
// ===========================================================================

TEST(PluginLifecycle, CreateFailureAfterStoringHandleDestroysAndClearsIt) {
    auto module = PluginModule::load(LEAKY_CREATE_FIXTURE);
    ASSERT_TRUE(module.has_value()) << module.error().message;

    // Second dlopen of the same module: shares the fixture's globals and
    // keeps the mapping alive for the control exports.
    auto control_library = SharedLibrary::open(LEAKY_CREATE_FIXTURE);
    ASSERT_TRUE(control_library.has_value()) << control_library.error();
    using SetMode = void (*)(int);
    using GetState = void (*)(uint64_t*, uintptr_t*);
    using Token = uintptr_t (*)();
    const auto set_mode = reinterpret_cast<SetMode>(control_library->resolve("leaky_create_set_mode"));      // NOLINT
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
    EXPECT_EQ(module->create(services, &handle, error), YADDNSC_STATUS_INTERNAL_ERROR);
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
    EXPECT_EQ(module->create(services, &handle, error), YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(std::string(error.message.data, error.message.size), "create exploded after storing a handle");

    get_state(&destroys, &last_destroyed);
    EXPECT_EQ(destroys, 2u);
    EXPECT_EQ(last_destroyed, token());
}

TEST(PluginLifecycle, ManualLoadFailsFastOnAbiMismatch) {
    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = false;
    settings.load.push_back(BAD_REVISION_FIXTURE);  // absolute path is used as-is

    EXPECT_THROW({ DriverLoader::load(catalog, settings); }, PluginLoadException);
    EXPECT_TRUE(catalog.get_loaded_drivers().empty());
}

TEST(PluginLifecycle, AutoDiscoverSkipsAbiMismatchedLibraries) {
    // One directory containing only the revision-mismatched fixture: auto
    // discovery must skip it with a warning instead of aborting startup.
    char dir_template[] = "/tmp/yaddnsc_plugin_test_XXXXXX";
    auto* dir = ::mkdtemp(dir_template);
    ASSERT_NE(dir, nullptr) << "mkdtemp failed";

    std::filesystem::copy_file(BAD_REVISION_FIXTURE, std::string(dir) + "/bad_revision.so");

    DriverCatalog catalog;
    domain::DriverSettings settings;
    settings.auto_discover = true;
    settings.driver_dir = dir;

    EXPECT_NO_THROW({ DriverLoader::load(catalog, settings); });
    EXPECT_TRUE(catalog.get_loaded_drivers().empty());

    std::filesystem::remove_all(dir);
}
