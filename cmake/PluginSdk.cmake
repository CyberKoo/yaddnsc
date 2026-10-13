# Pure C surface: no Glaze and no C++ language requirement.
add_library(yaddnsc_plugin_abi INTERFACE)
add_library(yaddnsc::plugin_abi ALIAS yaddnsc_plugin_abi)
set_target_properties(yaddnsc_plugin_abi PROPERTIES EXPORT_NAME plugin_abi)
target_include_directories(yaddnsc_plugin_abi INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/sdk_include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

configure_file(${PROJECT_SOURCE_DIR}/cmake/sdk_version.hpp.in
    ${PROJECT_BINARY_DIR}/generated/sdk_include/yaddnsc/sdk/version.hpp @ONLY)

# SDK interface for driver plugins — the ONLY supported surface: the v1
# C ABI plus the C++ helper layer (yaddnsc/sdk/*). Deliberately does NOT expose
# the host's src/ include root or host-only dependencies.
add_library(yaddnsc_plugin_sdk INTERFACE)
add_library(yaddnsc::plugin_sdk ALIAS yaddnsc_plugin_sdk)
set_target_properties(yaddnsc_plugin_sdk PROPERTIES EXPORT_NAME plugin_sdk)
target_include_directories(yaddnsc_plugin_sdk INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated/sdk_include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(yaddnsc_plugin_sdk INTERFACE yaddnsc_plugin_abi glaze::glaze)
target_compile_features(yaddnsc_plugin_sdk INTERFACE cxx_std_23)

# C11 compile of the v1 ABI header. An OBJECT library is not linked; a
# warning or a layout assert fails the build even when tests are disabled.
add_library(yaddnsc_c_abi_compile OBJECT test/plugin/c_abi_compile.c)
target_include_directories(yaddnsc_c_abi_compile PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(yaddnsc_c_abi_compile PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
target_compile_options(yaddnsc_c_abi_compile PRIVATE -Wall -Wextra -Wpedantic -Werror)

# XML is deliberately an opt-in SDK component.  driver.hpp itself does not
# include libxml2, so ordinary plugin consumers only need Glaze.  Drivers that
# include xml_raii.hpp link this target to obtain both the SDK and libxml2.
find_package(LibXml2 QUIET)
if (LibXml2_FOUND)
  add_library(yaddnsc_plugin_sdk_xml INTERFACE)
  add_library(yaddnsc::plugin_sdk_xml ALIAS yaddnsc_plugin_sdk_xml)
  set_target_properties(yaddnsc_plugin_sdk_xml PROPERTIES EXPORT_NAME plugin_sdk_xml)
  target_link_libraries(yaddnsc_plugin_sdk_xml INTERFACE yaddnsc_plugin_sdk LibXml2::LibXml2)
  set(YADDNSC_HAS_SDK_XML_COMPONENT ON)
else ()
  set(YADDNSC_HAS_SDK_XML_COMPONENT OFF)
endif ()

# Optional header-only signing helpers. Consumers compile with their own C++ ABI.
add_library(yaddnsc_plugin_sdk_crypto INTERFACE)
add_library(yaddnsc::plugin_sdk_crypto ALIAS yaddnsc_plugin_sdk_crypto)
set_target_properties(yaddnsc_plugin_sdk_crypto PROPERTIES EXPORT_NAME plugin_sdk_crypto)
target_link_libraries(yaddnsc_plugin_sdk_crypto INTERFACE yaddnsc_plugin_abi OpenSSL::Crypto)
target_compile_features(yaddnsc_plugin_sdk_crypto INTERFACE cxx_std_23)
