# ==============================================================================
# Plugin contract tests (v1 alpha C ABI)
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
foreach(fixture bad_revision bad_magic missing_symbol small_descriptor)
    add_library(yaddnsc_fixture_${fixture} MODULE fixtures/${fixture}.cpp)
    set_target_properties(yaddnsc_fixture_${fixture} PROPERTIES PREFIX "" OUTPUT_NAME "${fixture}")
    target_include_directories(yaddnsc_fixture_${fixture} PRIVATE ${PROJECT_SOURCE_DIR}/include)
endforeach()

# ---------------------------------------------------------------------------
# Optional-entry fixture — a complete, loadable plugin that predates the
# OPTIONAL yaddnsc_driver_validate entry. The loader must accept it and the
# host must skip the driver-side driver_param check instead of failing.
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

# ---------------------------------------------------------------------------
# Test binaries
# ---------------------------------------------------------------------------
add_unit_test(plugin_abi_contract SOURCE driver_abi_contract_test.cpp)
add_unit_test(plugin_lifecycle SOURCE plugin_lifecycle_test.cpp)
add_unit_test(abi_driver_gateway SOURCE abi_driver_gateway_test.cpp)

foreach(test_target test_plugin_abi_contract test_plugin_lifecycle test_abi_driver_gateway)
    target_link_libraries(${test_target} PRIVATE yaddnsc_prod_all ${CMAKE_DL_LIBS})
    # macOS + ASan + dlopen plugins: disable the ODR checker for the plugin's
    # vtables (same known false positive as test_driver_loader).
    set_tests_properties(${test_target} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")
    target_compile_definitions(${test_target} PRIVATE
        TEST_PLUGIN_PATH="$<TARGET_FILE:yaddnsc_test_driver_plugin>"
        BAD_REVISION_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_bad_revision>"
        BAD_MAGIC_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_bad_magic>"
        MISSING_SYMBOL_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_missing_symbol>"
        SMALL_DESCRIPTOR_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_small_descriptor>"
        NO_VALIDATE_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_no_validate>"
        THROWING_FIXTURE="$<TARGET_FILE:yaddnsc_fixture_throwing>")
    add_dependencies(${test_target}
        yaddnsc_test_driver_plugin
        yaddnsc_fixture_bad_revision
        yaddnsc_fixture_bad_magic
        yaddnsc_fixture_missing_symbol
        yaddnsc_fixture_small_descriptor
        yaddnsc_fixture_no_validate
        yaddnsc_fixture_throwing)
endforeach()
