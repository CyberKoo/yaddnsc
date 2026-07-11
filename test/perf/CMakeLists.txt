# ==============================================================================
# Performance benchmarks — Google Benchmark targets
#
# Each add_benchmark call creates an executable bench_<name> linked against
# benchmark::benchmark_main (provides main()) and test_support.
# ==============================================================================

function(add_benchmark name)
    cmake_parse_arguments(BENCH "NATIVE" "SOURCE" "SOURCES" ${ARGN})
    if (BENCH_SOURCE)
        set(srcs ${BENCH_SOURCE})
    else ()
        set(srcs ${name}_bench.cpp)
    endif ()
    if (BENCH_SOURCES)
        list(APPEND srcs ${BENCH_SOURCES})
    endif ()
    add_executable(bench_${name} ${srcs})
    target_link_libraries(bench_${name} PRIVATE
        benchmark::benchmark
        benchmark::benchmark_main
        test_support
        spdlog::spdlog
        magic_enum
        glaze::glaze
    )
    target_include_directories(bench_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_BINARY_DIR}/generated
    )
    if (BENCH_NATIVE)
        target_compile_definitions(bench_${name} PRIVATE YADDNSC_USE_NATIVE_DNS=1)
    endif ()

    # Benchmarks must run with optimisations even in Debug builds, otherwise
    # the measured timings are meaningless (Google Benchmark prints a warning).
    target_compile_options(bench_${name} PRIVATE
        $<$<NOT:$<CONFIG:Release>>:-O2 -DNDEBUG>
    )
endfunction()

# ============================================================================
#  DNS parser benchmarks  (same test code, two backends)
# ============================================================================

add_benchmark(dns_parser_native
    SOURCE dns_parser_bench.cpp
    SOURCES ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_native.cpp
    NATIVE
)

find_library(PERF_RESOLV resolv)
if (PERF_RESOLV)
  add_benchmark(dns_parser_system
      SOURCE dns_parser_bench.cpp
      SOURCES ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_system.cpp
  )
  target_link_libraries(bench_dns_parser_system PRIVATE ${PERF_RESOLV})
endif ()

# ============================================================================
#  URI benchmarks  (Uri is in test_support)
# ============================================================================

add_benchmark(uri)

# ============================================================================
#  String utility benchmarks  (header-only)
# ============================================================================

add_benchmark(string_util)

# ============================================================================
#  InetAddress benchmarks  (InetAddress is in test_support)
# ============================================================================

add_benchmark(inet_address)

# ============================================================================
#  SocketAddr benchmarks  (needs socket_addr.cpp)
# ============================================================================

add_benchmark(socket_addr
    SOURCES ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp
)

# ============================================================================
#  DNS QueryBuilder benchmarks  (needs builder.cpp)
# ============================================================================

add_benchmark(builder
    SOURCES ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp
)
