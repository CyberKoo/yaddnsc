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
set_target_properties(yaddnsc_plugin_crypto PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(yaddnsc_plugin_crypto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(yaddnsc_plugin_crypto PRIVATE OpenSSL::Crypto)
target_compile_options(yaddnsc_plugin_crypto PRIVATE -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)

# Sanitizer instrumentation (must match the drivers' flags)
add_sanitizer_flags(yaddnsc_plugin_crypto)
