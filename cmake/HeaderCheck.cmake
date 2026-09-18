# ==============================================================================
# Header self-containment checks
# ==============================================================================
# Compiles every first-party header as its own translation unit. A header
# that relies on transitive includes from its includers (the classic
# "builds with libstdc++, fails with libc++" bug) fails here on every
# platform, not just the unlucky one.
#
# One generated TU per header, all in a single OBJECT library so builds stay
# parallel. Part of the default build: cheap, and always enforced.
file(GLOB_RECURSE YADDNSC_HEADER_CHECK_FILES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/include/*.h
    ${PROJECT_SOURCE_DIR}/include/*.hpp
    ${PROJECT_SOURCE_DIR}/plugin_crypto/*.h
)

# xml_raii.hpp includes <libxml/parser.h>; libxml2 is an optional dependency
# (only the route53/namecheap drivers use it), so only check the header when
# the library is available.
find_package(LibXml2 QUIET)
if(NOT LibXml2_FOUND)
    list(FILTER YADDNSC_HEADER_CHECK_FILES EXCLUDE REGEX "xml_raii\\.hpp$")
endif()

set(YADDNSC_HEADER_CHECK_DIR ${CMAKE_BINARY_DIR}/generated/header_checks)
set(YADDNSC_HEADER_CHECK_SOURCES "")
foreach(header IN LISTS YADDNSC_HEADER_CHECK_FILES)
    string(MD5 tu_name ${header})
    set(tu ${YADDNSC_HEADER_CHECK_DIR}/check_${tu_name}.cpp)
    file(GENERATE OUTPUT ${tu} CONTENT "#include \"${header}\"\n")
    list(APPEND YADDNSC_HEADER_CHECK_SOURCES ${tu})
endforeach()

add_library(yaddnsc_header_checks OBJECT ${YADDNSC_HEADER_CHECK_SOURCES})
# The generated TUs are intentionally include-only; IWYU would flag the sole
# include as unused. Self-containment is this target's only job.
set_target_properties(yaddnsc_header_checks PROPERTIES CXX_INCLUDE_WHAT_YOU_USE "")
target_link_libraries(yaddnsc_header_checks PRIVATE
    yaddnsc_warnings
    yaddnsc_build_options
    yaddnsc_internal_headers
    yaddnsc_plugin_sdk
    OpenSSL::Crypto
    $<$<BOOL:${LibXml2_FOUND}>:LibXml2::LibXml2>
)
# Third-party headers referenced from first-party headers.
foreach(dep IN ITEMS BS_thread_pool spdlog::spdlog fmt::fmt)
    if(TARGET ${dep})
        target_link_libraries(yaddnsc_header_checks PRIVATE ${dep})
    endif()
endforeach()
add_sanitizer_flags(yaddnsc_header_checks)
