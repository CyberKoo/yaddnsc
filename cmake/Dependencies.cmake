# Third-party dependency management via FetchContent

include(FetchContent)

# ---------------------------------------------------------------------------
# glaze – JSON/reflection library
# ---------------------------------------------------------------------------
FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG        v7.8.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glaze)

# ---------------------------------------------------------------------------
# fmt / std::format  (conditional)
# ---------------------------------------------------------------------------
if (HAVE_STD_FORMAT)
    message(STATUS "Using native std::format")
    add_compile_definitions(YADDNSC_USE_STD_FORMAT)
    set(SPDLOG_USE_STD_FORMAT ON)
else ()
    message(STATUS "Using external fmt library")
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG        12.2.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(fmt)
    set(SPDLOG_FMT_EXTERNAL ON)
endif ()

# interface target so all internal consumers get the right dep
add_library(yaddnsc_fmt INTERFACE)
target_include_directories(yaddnsc_fmt INTERFACE ${CMAKE_SOURCE_DIR}/include)
if (NOT HAVE_STD_FORMAT)
    target_link_libraries(yaddnsc_fmt INTERFACE fmt::fmt)
endif ()

# ---------------------------------------------------------------------------
# spdlog – logging library
# ---------------------------------------------------------------------------
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)
set_target_properties(spdlog PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

# ---------------------------------------------------------------------------
# cpp-httplib – HTTP client/server
# ---------------------------------------------------------------------------
add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)
set(HTTPLIB_REQUIRE_OPENSSL ON)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.47.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(httplib)

# ---------------------------------------------------------------------------
# cxxopts – command line parser
# ---------------------------------------------------------------------------
FetchContent_Declare(
    cxxopts
    GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
    GIT_TAG        v3.3.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cxxopts)

# ---------------------------------------------------------------------------
# BS::thread_pool – header-only, no CMakeLists.txt
# Policy CMP0169: FetchContent_Populate with declared details is deprecated
# but needed here because thread-pool has no CMakeLists.txt for MakeAvailable.
# ---------------------------------------------------------------------------
cmake_policy(PUSH)
cmake_policy(SET CMP0169 OLD)
FetchContent_Declare(
    thread-pool
    GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
    GIT_TAG        v5.1.0
)
FetchContent_GetProperties(thread-pool)
if(NOT thread-pool_POPULATED)
    FetchContent_Populate(thread-pool)
    add_library(BS_thread_pool INTERFACE)
    target_include_directories(BS_thread_pool INTERFACE ${thread-pool_SOURCE_DIR}/include)
endif()
cmake_policy(POP)
