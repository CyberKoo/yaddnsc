# ==============================================================================
# Shared build configuration — three focused concerns
# ==============================================================================

# Interface target: wraps fmt so consumers always get the right dep and define.
# The facade header itself is host-internal under src/.
add_library(yaddnsc_fmt INTERFACE)
target_compile_definitions(yaddnsc_fmt INTERFACE
        $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
)
if (NOT HAVE_STD_FORMAT)
  target_link_libraries(yaddnsc_fmt INTERFACE fmt::fmt)
endif ()

# ---------------------------------------------------------------------------
# yaddnsc_warnings — strict warnings for production and test targets
# ---------------------------------------------------------------------------
add_library(yaddnsc_warnings INTERFACE)
target_compile_options(yaddnsc_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
    # GCC 14 with -O1 (Sanitizer build type) produces false-positive
    # -Wmaybe-uninitialized warnings in <regex> internals (std::function
    # move constructor in std::__detail::_State). -Wall enables this
    # warning, and -Werror promotes it to a hard error.  Suppress it
    # for Sanitizer builds where -O1 is required for instrumentation.
    $<$<CONFIG:Sanitizer>:-Wno-maybe-uninitialized>
)

# System fmt < 10 (Ubuntu 24.04 ships 9.1.0) hits a GCC 14 false-positive
# -Warray-bounds inside fmt's bigint (dragon float formatting path,
# fmtlib/fmt#3731, fixed in fmt 10). -Werror turns it into a hard error;
# demote the warning for this combination only so real array-bounds bugs
# in project code still surface as warnings.
if (YADDNSC_USE_SYSTEM_SPDLOG AND fmt_VERSION VERSION_LESS 10)
  target_compile_options(yaddnsc_warnings INTERFACE -Wno-error=array-bounds)
endif ()

# ---------------------------------------------------------------------------
# Include-What-You-Use (cmake/IWYU.cmake). Enabled here — after every CPM
# dependency target exists — so only first-party targets (production modules,
# drivers, plugin support libraries, tests) inherit the check; third-party TUs are never
# analyzed.
# ---------------------------------------------------------------------------
if (YADDNSC_IWYU_COMMAND)
  set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE ${YADDNSC_IWYU_COMMAND})
endif ()

# ---------------------------------------------------------------------------
# yaddnsc_build_options — language level, visibility, project-wide compile
# definitions and include roots. The only public headers are the plugin SDK
# under include/yaddnsc/; host headers live under src/.
# ---------------------------------------------------------------------------
add_library(yaddnsc_build_options INTERFACE)
target_compile_options(yaddnsc_build_options INTERFACE
    -fvisibility=hidden
    $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,19>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,20>>:-fexperimental-library>
    $<$<CONFIG:Debug>:-ggdb>
    $<$<CONFIG:Debug>:-gdwarf-3>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
)
target_compile_definitions(yaddnsc_build_options INTERFACE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
    $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
    $<$<BOOL:${YADDNSC_USE_SYSTEM_SPDLOG}>:YADDNSC_USE_SYSTEM_SPDLOG>
)
target_include_directories(yaddnsc_build_options INTERFACE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)

# ---------------------------------------------------------------------------
# yaddnsc_sanitizers — per-build-type sanitizer flags (cmake/Sanitizer.cmake),
# linked via add_sanitizer_flags(<target>).
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# yaddnsc_internal_headers — the host-internal src/ include root.
# Module headers reference each other through src/-rooted paths
# (e.g. "domain/network/inet_address.h"). PRIVATE to production targets; never
# exposed to driver plugins.
# ---------------------------------------------------------------------------
add_library(yaddnsc_internal_headers INTERFACE)
target_include_directories(yaddnsc_internal_headers INTERFACE ${PROJECT_SOURCE_DIR}/src)

# Baseline shared by every production module target.
function(yaddnsc_production_module TARGET)
  target_link_libraries(${TARGET} PRIVATE
      yaddnsc_warnings
      yaddnsc_build_options
      yaddnsc_internal_headers
  )
  add_sanitizer_flags(${TARGET})
endfunction()
