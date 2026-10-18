# ==============================================================================
# Plugin contract tests (v1 C ABI)
#
# A whiteboard test plugin (real MODULE .so, dlopen'ed by the tests) plus
# deliberately broken fixture libraries for the loader rejection matrix and
# one plugin that predates the optional validate entry. The test binaries
# link the production plugin host and drive the plugins through their C
# entry points / the AbiDriverGateway.
# ==============================================================================

# ---------------------------------------------------------------------------
# Whiteboard contract-test plugin — built like a real driver: SDK only, no
# host-internal headers, sanitizer-instrumented to match the test binaries.
# ---------------------------------------------------------------------------
add_library(yaddnsc_test_driver_plugin MODULE test_driver_plugin.cpp)
set_target_properties(yaddnsc_test_driver_plugin PROPERTIES PREFIX "" OUTPUT_NAME "test_driver_plugin")
target_link_libraries(yaddnsc_test_driver_plugin PRIVATE yaddnsc_plugin_sdk)
add_sanitizer_flags(yaddnsc_test_driver_plugin)

# ---------------------------------------------------------------------------
# Loader-rejection fixtures — raw C ABI, each broken in exactly one dimension.
# Like yaddnsc_bad_driver_fixture these are intentionally minimal and not
# sanitizer-instrumented.
# ---------------------------------------------------------------------------
foreach(fixture bad_revision newer_minor bad_magic missing_symbol small_descriptor short_descriptor)
    add_library(yaddnsc_fixture_${fixture} MODULE fixtures/${fixture}.cpp)
    set_target_properties(yaddnsc_fixture_${fixture} PROPERTIES PREFIX "" OUTPUT_NAME "${fixture}")
    target_include_directories(yaddnsc_fixture_${fixture} PRIVATE ${PROJECT_SOURCE_DIR}/include)
endforeach()

foreach(field name version author description capabilities)
    add_library(yaddnsc_fixture_invalid_${field} MODULE fixtures/invalid_descriptor.cpp)
    set_target_properties(yaddnsc_fixture_invalid_${field} PROPERTIES PREFIX "" OUTPUT_NAME "invalid_${field}")
    target_include_directories(yaddnsc_fixture_invalid_${field} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    string(TOUPPER "${field}" field_upper)
    target_compile_definitions(yaddnsc_fixture_invalid_${field} PRIVATE "YADDNSC_FIXTURE_INVALID_${field_upper}")
endforeach()

# ---------------------------------------------------------------------------
# Optional-entry fixture — a complete, loadable plugin that predates the
# OPTIONAL yaddnsc_driver_validate entry. The loader must accept it, and
# config test must fail closed: the host cannot confirm driver_params.
# ---------------------------------------------------------------------------
add_library(yaddnsc_fixture_no_validate MODULE fixtures/no_validate.cpp)
set_target_properties(yaddnsc_fixture_no_validate PROPERTIES PREFIX "" OUTPUT_NAME "no_validate")
target_include_directories(yaddnsc_fixture_no_validate PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Exception-firewall fixture — a loadable plugin whose entry points throw
# across the C ABI; the host trampolines must translate each escape into
# YADDNSC_STATUS_INTERNAL_ERROR instead of crashing.
add_library(yaddnsc_fixture_throwing MODULE fixtures/throwing.cpp)
set_target_properties(yaddnsc_fixture_throwing PROPERTIES PREFIX "" OUTPUT_NAME "throwing")
target_include_directories(yaddnsc_fixture_throwing PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Destroy-firewall fixture — destroy() throws a non-std type; the catch-all
# arm of the host's destroy firewall must swallow it (log only, no terminate).
add_library(yaddnsc_fixture_throwing_destroy MODULE fixtures/throwing_destroy.cpp)
set_target_properties(yaddnsc_fixture_throwing_destroy PROPERTIES PREFIX "" OUTPUT_NAME "throwing_destroy")
target_include_directories(yaddnsc_fixture_throwing_destroy PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Create-contract fixture — create() stores a handle and THEN fails/throws;
# the host must destroy and clear the leaked handle before returning.
add_library(yaddnsc_fixture_leaky_create MODULE fixtures/leaky_create.cpp)
set_target_properties(yaddnsc_fixture_leaky_create PROPERTIES PREFIX "" OUTPUT_NAME "leaky_create")
target_include_directories(yaddnsc_fixture_leaky_create PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Async destroy-firewall fixture — the async destroy entry throws; the host
# must apply the fatal cleanup policy rather than release still-borrowed state.
add_library(yaddnsc_fixture_async_throwing_destroy MODULE fixtures/async_throwing_destroy.cpp)
set_target_properties(yaddnsc_fixture_async_throwing_destroy PROPERTIES PREFIX "" OUTPUT_NAME "async_throwing_destroy")
target_include_directories(yaddnsc_fixture_async_throwing_destroy PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_library(yaddnsc_fixture_async_create_cleanup MODULE fixtures/async_create_cleanup.cpp)
set_target_properties(yaddnsc_fixture_async_create_cleanup PROPERTIES PREFIX "" OUTPUT_NAME "async_create_cleanup")
target_include_directories(yaddnsc_fixture_async_create_cleanup PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_sanitizer_flags(yaddnsc_fixture_async_create_cleanup)

# Create-contract fixture — create() returns without storing a handle, either
# with OK or with a failure status. The host must reject OK+NULL and must not
# destroy a pointer the caller had stashed in *out_driver.
add_library(yaddnsc_fixture_create_contract MODULE fixtures/create_contract.cpp)
set_target_properties(yaddnsc_fixture_create_contract PROPERTIES PREFIX "" OUTPUT_NAME "create_contract")
target_include_directories(yaddnsc_fixture_create_contract PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Validate-time HTTP fixture — validate() calls the services table captured
# at create(). The host table for config validation must refuse the exchange.
add_library(yaddnsc_fixture_validate_http MODULE fixtures/validate_http.cpp)
set_target_properties(yaddnsc_fixture_validate_http PROPERTIES PREFIX "" OUTPUT_NAME "validate_http")
target_include_directories(yaddnsc_fixture_validate_http PRIVATE ${PROJECT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Frozen ABI 1.0 baseline plugin — compiles ONLY against the frozen v1.0
# header copy (abi_baseline/v1_0/), never the live include/ tree. The copy is
# sha256-guarded in CI. This is the freeze tripwire: a host-side change that
# breaks ABI 1.0 compatibility turns test_abi_freeze_compat red.
# ---------------------------------------------------------------------------
add_library(yaddnsc_fixture_frozen_v1_0 MODULE abi_baseline/frozen_v1_0_plugin.cpp)
set_target_properties(yaddnsc_fixture_frozen_v1_0 PROPERTIES PREFIX "" OUTPUT_NAME "frozen_v1_0")
target_include_directories(yaddnsc_fixture_frozen_v1_0 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/abi_baseline/v1_0)

# ---------------------------------------------------------------------------
# Test binaries
# ---------------------------------------------------------------------------
add_unit_test(plugin_abi_contract SOURCE driver_abi_contract_test.cpp)
add_unit_test(plugin_lifecycle SOURCE plugin_lifecycle_test.cpp)

# Frozen ABI 1.0 baseline — dlopen the frozen-header plugin and drive a full
# cycle through the production PluginModule.
add_unit_test(abi_freeze_compat SOURCE abi_freeze_compat_test.cpp)
target_link_libraries(test_abi_freeze_compat PRIVATE yaddnsc_prod_all ${CMAKE_DL_LIBS})
set_tests_properties(test_abi_freeze_compat PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")
target_compile_definitions(test_abi_freeze_compat PRIVATE
    FROZEN_V1_0_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_frozen_v1_0>")
add_dependencies(test_abi_freeze_compat yaddnsc_fixture_frozen_v1_0)

# Coroutine driver gateway — drives the same whiteboard plugin through the
# coroutine bridge (worker ↔ loop HTTP, concurrent cycles, abandon, shutdown
# cancel) against an in-process HTTP server.
add_unit_test(coro_driver_gateway SOURCE coro_driver_gateway_test.cpp)
target_link_libraries(test_coro_driver_gateway PRIVATE yaddnsc_prod_all yaddnsc_coro_plugin ${CMAKE_DL_LIBS})
set_tests_properties(test_coro_driver_gateway PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2"
    TIMEOUT 120)
target_compile_definitions(test_coro_driver_gateway PRIVATE
    TEST_PLUGIN_PATH="$<TARGET_FILE:yaddnsc_test_driver_plugin>"
    NO_VALIDATE_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_no_validate>"
    THROWING_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_throwing>"
    ASYNC_CREATE_FAILS_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_async_create_fails>")
add_dependencies(test_coro_driver_gateway yaddnsc_test_driver_plugin yaddnsc_fixture_no_validate
    yaddnsc_fixture_throwing yaddnsc_fixture_async_create_fails)

# ABI error-report contract — drives a plugin that breaks one ABI rule at a time
# (create returning OK without a handle, a non-OK status with no error report,
# a report whose status disagrees, an overflowing retry_after, a plugin-reported
# CANCELLED) through the coroutine gateway. No mode reaches the HTTP bridge, so
# these tests need no server.
add_library(yaddnsc_fixture_abi_misbehave MODULE fixtures/abi_misbehave.cpp)
set_target_properties(yaddnsc_fixture_abi_misbehave PROPERTIES PREFIX "" OUTPUT_NAME "abi_misbehave")
target_include_directories(yaddnsc_fixture_abi_misbehave PRIVATE ${PROJECT_SOURCE_DIR}/include)

# Capability-bit fixtures — one declares only the AAAA bit, one declares none.
# The catalog translates the descriptor's capability bits into the record kinds
# the application layer sees, and each bit must stand on its own.
foreach(caps aaaa_only no_caps)
    add_library(yaddnsc_fixture_${caps} MODULE fixtures/${caps}.cpp)
    set_target_properties(yaddnsc_fixture_${caps} PROPERTIES PREFIX "" OUTPUT_NAME "${caps}")
    target_include_directories(yaddnsc_fixture_${caps} PRIVATE ${PROJECT_SOURCE_DIR}/include)
endforeach()
add_unit_test(gateway_abi_contract SOURCE gateway_abi_contract_test.cpp)
target_link_libraries(test_gateway_abi_contract PRIVATE yaddnsc_prod_all yaddnsc_coro_plugin ${CMAKE_DL_LIBS})
set_tests_properties(test_gateway_abi_contract PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")
target_compile_definitions(test_gateway_abi_contract PRIVATE
    ABI_MISBEHAVE_PATH="$<TARGET_FILE:yaddnsc_fixture_abi_misbehave>")
add_dependencies(test_gateway_abi_contract yaddnsc_fixture_abi_misbehave)

foreach(test_target test_plugin_abi_contract test_plugin_lifecycle)
    target_link_libraries(${test_target} PRIVATE yaddnsc_prod_all ${CMAKE_DL_LIBS})
    # macOS + ASan + dlopen plugins: disable the ODR checker for the plugin's
    # vtables (same known false positive as test_driver_loader).
    set_tests_properties(${test_target} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")
    target_compile_definitions(${test_target} PRIVATE
        TEST_PLUGIN_PATH="$<TARGET_FILE:yaddnsc_test_driver_plugin>"
        BAD_REVISION_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_bad_revision>"
        NEWER_MINOR_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_newer_minor>"
        BAD_MAGIC_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_bad_magic>"
        MISSING_SYMBOL_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_missing_symbol>"
        SMALL_DESCRIPTOR_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_small_descriptor>"
        SHORT_DESCRIPTOR_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_short_descriptor>"
        INVALID_NAME_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_invalid_name>"
        INVALID_VERSION_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_invalid_version>"
        INVALID_AUTHOR_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_invalid_author>"
        INVALID_DESCRIPTION_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_invalid_description>"
        INVALID_CAPABILITIES_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_invalid_capabilities>"
        NO_VALIDATE_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_no_validate>"
        THROWING_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_throwing>"
        THROWING_DESTROY_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_throwing_destroy>"
        LEAKY_CREATE_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_leaky_create>"
        CREATE_CONTRACT_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_create_contract>"
        VALIDATE_HTTP_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_validate_http>"
        ASYNC_THROWING_DESTROY_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_async_throwing_destroy>")
    add_dependencies(${test_target}
        yaddnsc_test_driver_plugin
        yaddnsc_fixture_bad_revision
        yaddnsc_fixture_newer_minor
        yaddnsc_fixture_bad_magic
        yaddnsc_fixture_missing_symbol
        yaddnsc_fixture_small_descriptor
        yaddnsc_fixture_short_descriptor
        yaddnsc_fixture_invalid_name
        yaddnsc_fixture_invalid_version
        yaddnsc_fixture_invalid_author
        yaddnsc_fixture_invalid_description
        yaddnsc_fixture_invalid_capabilities
        yaddnsc_fixture_no_validate
        yaddnsc_fixture_throwing
        yaddnsc_fixture_throwing_destroy
        yaddnsc_fixture_leaky_create
        yaddnsc_fixture_create_contract
        yaddnsc_fixture_validate_http
        yaddnsc_fixture_async_throwing_destroy)
endforeach()

# Separate allocator rendezvous: does not affect dlopen or other test binaries.
add_unit_test(plugin_cancellation_publication SOURCE cancellation_publication_test.cpp)
target_link_libraries(test_plugin_cancellation_publication PRIVATE yaddnsc_coro_plugin test_allocation_fault)
target_compile_definitions(test_plugin_cancellation_publication PRIVATE
    TEST_PLUGIN_PATH="$<TARGET_FILE:yaddnsc_test_driver_plugin>"
)
add_dependencies(test_plugin_cancellation_publication yaddnsc_test_driver_plugin)
set_tests_properties(test_plugin_cancellation_publication PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2"
    TIMEOUT 20)

add_library(yaddnsc_fixture_phase_http MODULE fixtures/phase_http.cpp)
target_link_libraries(yaddnsc_fixture_phase_http PRIVATE yaddnsc_plugin_sdk)
set_target_properties(yaddnsc_fixture_phase_http PROPERTIES PREFIX "" OUTPUT_NAME "phase_http")
add_sanitizer_flags(yaddnsc_fixture_phase_http)
target_compile_definitions(test_coro_driver_gateway PRIVATE PHASE_HTTP_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_phase_http>")
add_dependencies(test_coro_driver_gateway yaddnsc_fixture_phase_http)

# ---------------------------------------------------------------------------
# ABI 1.1 async interface — a pure C ABI fixture (no SDK, no sync entries)
# plus a direct contract test. This binary drives the fixture's async entry
# points itself; loader mode selection and the production gateway are covered
# by the lifecycle and coroutine-gateway tests.
# ---------------------------------------------------------------------------
add_library(yaddnsc_fixture_async MODULE fixtures/async_plugin.cpp)
set_target_properties(yaddnsc_fixture_async PROPERTIES PREFIX "" OUTPUT_NAME "async_plugin")
target_include_directories(yaddnsc_fixture_async PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_sanitizer_flags(yaddnsc_fixture_async)

add_library(yaddnsc_fixture_async_create_fails MODULE fixtures/async_create_fails.cpp)
set_target_properties(yaddnsc_fixture_async_create_fails PROPERTIES PREFIX "" OUTPUT_NAME "async_create_fails")
target_include_directories(yaddnsc_fixture_async_create_fails PRIVATE ${PROJECT_SOURCE_DIR}/include)

add_unit_test(async_abi_contract SOURCE async_abi_contract_test.cpp)
target_link_libraries(test_async_abi_contract PRIVATE yaddnsc_prod_all ${CMAKE_DL_LIBS})
target_compile_definitions(test_async_abi_contract PRIVATE
    TEST_ASYNC_PLUGIN_PATH="$<TARGET_FILE:yaddnsc_fixture_async>")
add_dependencies(test_async_abi_contract yaddnsc_fixture_async)
set_tests_properties(test_async_abi_contract PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")

# ---------------------------------------------------------------------------
# Loader mode-selection fixtures. One source, compiled with the descriptor
# minimum and the exported groups selected by CMake.
# ---------------------------------------------------------------------------
function(add_loader_mode_fixture target output minor sync async async_partial sync_validate_only)
    add_library(${target} MODULE fixtures/loader_mode_fixture.cpp)
    set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "${output}")
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE
        YADDNSC_FIXTURE_MINOR=${minor}
        YADDNSC_FIXTURE_SYNC=${sync}
        YADDNSC_FIXTURE_ASYNC=${async}
        YADDNSC_FIXTURE_ASYNC_PARTIAL=${async_partial}
        YADDNSC_FIXTURE_SYNC_VALIDATE_ONLY=${sync_validate_only})
endfunction()

add_loader_mode_fixture(yaddnsc_fixture_sync_minor_one sync_minor_one 1 1 0 0 0)
add_loader_mode_fixture(yaddnsc_fixture_async_mixed async_mixed 1 1 1 0 0)
add_loader_mode_fixture(yaddnsc_fixture_async_incomplete async_incomplete 1 0 0 1 0)
add_loader_mode_fixture(yaddnsc_fixture_async_old_minor async_old_minor 0 0 1 0 0)
add_loader_mode_fixture(yaddnsc_fixture_sync_legacy_extra_async sync_legacy_extra_async 0 1 0 1 0)
add_loader_mode_fixture(yaddnsc_fixture_async_with_sync_validate async_with_sync_validate 1 0 1 0 1)

target_compile_definitions(test_async_abi_contract PRIVATE
    SYNC_MINOR_ONE_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_sync_minor_one>"
    ASYNC_MIXED_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_async_mixed>"
    ASYNC_INCOMPLETE_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_async_incomplete>"
    ASYNC_OLD_MINOR_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_async_old_minor>"
    SYNC_LEGACY_EXTRA_ASYNC_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_sync_legacy_extra_async>"
    ASYNC_WITH_SYNC_VALIDATE_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_async_with_sync_validate>")
add_dependencies(test_async_abi_contract
    yaddnsc_fixture_sync_minor_one
    yaddnsc_fixture_async_mixed
    yaddnsc_fixture_async_incomplete
    yaddnsc_fixture_async_old_minor
    yaddnsc_fixture_sync_legacy_extra_async
    yaddnsc_fixture_async_with_sync_validate)

target_compile_definitions(test_coro_driver_gateway PRIVATE
    TEST_ASYNC_PLUGIN_PATH="$<TARGET_FILE:yaddnsc_fixture_async>")
add_dependencies(test_coro_driver_gateway yaddnsc_fixture_async)

# ---------------------------------------------------------------------------
# Async SDK fixture — a real AsyncDriver using the sequential exchange API.
# ---------------------------------------------------------------------------
add_library(yaddnsc_fixture_async_sdk MODULE fixtures/async_sdk_plugin.cpp)
set_target_properties(yaddnsc_fixture_async_sdk PROPERTIES PREFIX "" OUTPUT_NAME "async_sdk_driver")
target_link_libraries(yaddnsc_fixture_async_sdk PRIVATE yaddnsc_plugin_sdk)
add_sanitizer_flags(yaddnsc_fixture_async_sdk)

target_compile_definitions(test_coro_driver_gateway PRIVATE
    ASYNC_SDK_DRIVER_PATH="$<TARGET_FILE:yaddnsc_fixture_async_sdk>")
add_dependencies(test_coro_driver_gateway yaddnsc_fixture_async_sdk)

add_library(yaddnsc_fixture_async_slow_resume MODULE fixtures/async_slow_resume.cpp)
set_target_properties(yaddnsc_fixture_async_slow_resume PROPERTIES PREFIX "" OUTPUT_NAME "async_slow_resume")
target_include_directories(yaddnsc_fixture_async_slow_resume PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_sanitizer_flags(yaddnsc_fixture_async_slow_resume)
target_compile_definitions(test_coro_driver_gateway PRIVATE
    ASYNC_SLOW_RESUME_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_async_slow_resume>")
add_dependencies(test_coro_driver_gateway yaddnsc_fixture_async_slow_resume)

target_compile_definitions(test_plugin_lifecycle PRIVATE
    ASYNC_CREATE_CLEANUP_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_async_create_cleanup>")
add_dependencies(test_plugin_lifecycle yaddnsc_fixture_async_create_cleanup)
