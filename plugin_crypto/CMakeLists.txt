cmake_minimum_required(VERSION 3.28...3.31 FATAL_ERROR)

# ==============================================================================
# yaddnsc_plugin_crypto — cryptographic primitives for driver plugins
#
# Static library with PIC, linked into driver .so modules (route53,
# alibaba_cloud). Never linked into the host executable. Symbols are
# visibility-hidden so only the four ABI entry points remain exported
# from each plugin.
# ==============================================================================
add_library(yaddnsc_plugin_crypto STATIC signing.cpp)
add_library(yaddnsc::plugin_crypto ALIAS yaddnsc_plugin_crypto)
set_target_properties(yaddnsc_plugin_crypto PROPERTIES
    EXPORT_NAME plugin_crypto
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(yaddnsc_plugin_crypto PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/yaddnsc/plugin_crypto>
)
# This is a static library.  Its OpenSSL symbols are resolved by the final
# driver module, so the dependency must remain visible to package consumers.
target_link_libraries(yaddnsc_plugin_crypto PUBLIC OpenSSL::Crypto)
target_compile_options(yaddnsc_plugin_crypto PRIVATE -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)

# Sanitizer instrumentation (must match the drivers' flags)
add_sanitizer_flags(yaddnsc_plugin_crypto)
