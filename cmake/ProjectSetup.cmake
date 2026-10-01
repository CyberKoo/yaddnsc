# ==============================================================================
# Project-wide compiler, platform, feature and user configuration
# ==============================================================================

# Compiler & project-wide settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

include(ClangTidy)
include(IWYU)

# Default to Debug for developer experience (debug symbols, sanitizers).
# Use -DCMAKE_BUILD_TYPE=Release for optimized production builds.
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif ()

# Compiler version requirements
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "14")
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old. yaddnsc requires GCC 14+ (C++23).")
  endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19")
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old. yaddnsc requires Clang 19+ (C++23 + libc++ experimental features).")
  endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "15")
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old. yaddnsc requires AppleClang 15+ (C++23).")
  endif ()
else ()
  message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. yaddnsc requires GCC 14+, Clang 19+, or AppleClang 15+.")
endif ()

# The v1 alpha plugin ABI is LP64. driver_abi.h static_asserts the same layout
# for plugins built outside this project.
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "yaddnsc requires a 64-bit target (sizeof(void*) == 8). This compiler's pointer width is ${CMAKE_SIZEOF_VOID_P} bytes.")
endif ()

# Feature detection
include(FeatureCheck)

# Propagate to the CMake variable expected by config_cmake.h.in
# (#cmakedefine YADDNSC_LIBC_MUSL)
set(YADDNSC_LIBC_MUSL ${LIBC_MUSL})

# Debug-mode sanitizers can be disabled by downstream CI jobs (e.g. coverage)
# that need to combine Debug with incompatible flags like --coverage.
# Defined before Sanitizer.cmake, which evaluates it at include time.
option(YADDNSC_SANITIZE_DEBUG "Enable sanitizers in Debug builds" ON)
include(Sanitizer)

# Version info
# Version is managed by project(yaddnsc VERSION 1.0.0). The version.h.in
# template uses @PROJECT_VERSION_MAJOR@ etc. directly.
option(YADDNSC_DEVELOPMENT "Development mode (embed git version info)" ON)
include(ReleaseInfo)

# User options (cache variables, overridable via -D)
set(YADDNSC_LOGGING_PATTERN "[%D %T.%e] [%^%8l%$] [%8!t] [%15!s:%-4#] %v" CACHE STRING "Logging pattern passed to spdlog::set_pattern()")
set(YADDNSC_MIN_UPDATE_INTERVAL 60 CACHE STRING "Minimum allowed update interval (seconds)")
set(YADDNSC_DEFAULT_DNS_SERVER "1.1.1.1" CACHE STRING "Default DNS server address when none is configured")
set(YADDNSC_DEFAULT_DNS_PORT 53 CACHE STRING "Default DNS server port when none is configured")
option(YADDNSC_USE_SYSTEM_SPDLOG "Use spdlog from the system instead of the bundled CPM version" OFF)

if (YADDNSC_MIN_UPDATE_INTERVAL LESS 0)
  message(FATAL_ERROR "YADDNSC_MIN_UPDATE_INTERVAL must not be negative, got ${YADDNSC_MIN_UPDATE_INTERVAL}")
endif ()

# Validate YADDNSC_DEFAULT_DNS_SERVER — must be a valid IPv4 or IPv6 address.
include(ValidateIpAddress)
validate_ip_address("${YADDNSC_DEFAULT_DNS_SERVER}" "YADDNSC_DEFAULT_DNS_SERVER")

# Build ID — compiler identity fields used by build_id.hpp
set(YADDNSC_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(YADDNSC_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(YADDNSC_BUILD_ID_STR "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_BUILD_TYPE}")

include(GNUInstallDirs)
