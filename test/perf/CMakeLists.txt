# ==============================================================================
# Performance benchmarks — Google Benchmark targets
#
# Each add_benchmark call creates an executable bench_<name> linked against
# benchmark::benchmark_main (provides main()) and test_support.
# ==============================================================================

function(add_benchmark name)
    cmake_parse_arguments(BENCH "" "SOURCE" "SOURCES" ${ARGN})
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
    # Production module targets are sanitizer-instrumented; benchmarks that
    # link them need matching flags.
    add_sanitizer_flags(bench_${name})
    target_include_directories(bench_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_BINARY_DIR}/generated
    )

    # Benchmarks must run with optimisations even in Debug builds, otherwise
    # the measured timings are meaningless (Google Benchmark prints a warning).
    target_compile_options(bench_${name} PRIVATE
        $<$<NOT:$<CONFIG:Release>>:-O2 -DNDEBUG>
    )
endfunction()

# ============================================================================
#  DNS parser benchmark
# ============================================================================

add_benchmark(dns_parser)
target_link_libraries(bench_dns_parser PRIVATE yaddnsc_dns_infrastructure)

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
#  SocketAddr benchmarks
# ============================================================================

add_benchmark(socket_addr)
target_link_libraries(bench_socket_addr PRIVATE yaddnsc_network_infrastructure)

# ============================================================================
#  DNS QueryBuilder benchmarks
# ============================================================================

add_benchmark(builder)
target_link_libraries(bench_builder PRIVATE yaddnsc_dns_infrastructure)
