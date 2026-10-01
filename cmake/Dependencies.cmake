# ==============================================================================
# Dependencies (via CPM.cmake)
# ==============================================================================
include(CPM)

find_package(OpenSSL 3.0 REQUIRED)

# ---------------------------------------------------------------------------
# glaze – JSON/reflection library
# ---------------------------------------------------------------------------
# The installed C++ SDK exposes Glaze through yaddnsc::plugin_sdk.  CPM adds
# Glaze as a subproject, where its default is not to install package metadata;
# enable that metadata so an installed SDK never points consumers at this build
# tree's CPM cache.
set(glaze_INSTALL ON CACHE BOOL "Install Glaze package metadata for the yaddnsc SDK" FORCE)
CPMAddPackage(
    NAME glaze
    GITHUB_REPOSITORY stephenberry/glaze
    GIT_TAG v7.8.2
    EXCLUDE_FROM_ALL NO
    SYSTEM YES
    OPTIONS "glaze_INSTALL ON"
)

# ---------------------------------------------------------------------------
# fmt / std::format  (conditional)
# ---------------------------------------------------------------------------
if (YADDNSC_USE_SYSTEM_SPDLOG)
  # System spdlog links against libfmt internally, so we must use the system
  # fmt library too.  std::format (C++23) is deliberately NOT used here because
  # mixing it with libfmt's type system (join_view, etc.) causes silent
  # incompatibilities and build failures.
  if (HAVE_STD_FORMAT)
    message(STATUS "Using system fmt (std::format is available but disabled — "
                "system spdlog requires libfmt)")
  else ()
    message(STATUS "Using system fmt library")
  endif ()
  find_package(fmt REQUIRED)
elseif (HAVE_STD_FORMAT)
  message(STATUS "Using native std::format")
  set(SPDLOG_USE_STD_FORMAT ON)
  set(YADDNSC_USE_STD_FORMAT ON)
else ()
  message(STATUS "Using external fmt library via CPM")
  CPMAddPackage("gh:fmtlib/fmt#12.2.0")
  set(SPDLOG_FMT_EXTERNAL ON)
endif ()

# ---------------------------------------------------------------------------
# spdlog – logging library
# ---------------------------------------------------------------------------
if (YADDNSC_USE_SYSTEM_SPDLOG)
  find_package(spdlog REQUIRED)
  message(STATUS "Using system spdlog (${spdlog_VERSION})")
else ()
  CPMAddPackage("gh:gabime/spdlog@1.17.0")
  set_target_properties(spdlog PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif ()

# ---------------------------------------------------------------------------
# CLI11 – command line parser
# ---------------------------------------------------------------------------
CPMAddPackage("gh:CLIUtils/CLI11@2.6.2")

# ---------------------------------------------------------------------------
# magic_enum – static reflection for enums (header-only)
# ---------------------------------------------------------------------------
CPMAddPackage("gh:Neargye/magic_enum@0.9.8")

# ---------------------------------------------------------------------------
# picohttpparser – HTTP request/response parser (C library)
# ---------------------------------------------------------------------------
CPMAddPackage("gh:h2o/picohttpparser@1.2")
if (picohttpparser_ADDED)
  add_library(picohttpparser STATIC ${picohttpparser_SOURCE_DIR}/picohttpparser.c)
  target_include_directories(picohttpparser PUBLIC ${picohttpparser_SOURCE_DIR})
endif ()

# ---------------------------------------------------------------------------
# BS::thread_pool – header-only, no CMakeLists.txt
# ---------------------------------------------------------------------------
CPMAddPackage(
        URI "gh:bshoshany/thread-pool@5.1.0"
        DOWNLOAD_ONLY YES
)
if (thread-pool_ADDED)
  add_library(BS_thread_pool INTERFACE)
  target_include_directories(BS_thread_pool INTERFACE ${thread-pool_SOURCE_DIR}/include)
endif ()
