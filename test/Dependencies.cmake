# Test-only third-party dependencies. The root CMakeLists includes this file
# before BuildOptions enables static analysis for first-party targets.

include(${CMAKE_SOURCE_DIR}/cmake/CPM.cmake)

# ---------------------------------------------------------------------------
# GoogleTest  (v1.17.0 — compatible with C++23, includes gmock)
# ---------------------------------------------------------------------------
CPMAddPackage(
    URI "gh:google/googletest@1.17.0"
    OPTIONS "INSTALL_GTEST OFF"
             "gtest_force_shared_crt ON"
)

if (googletest_ADDED)
    message(STATUS "GoogleTest v${googletest_VERSION} loaded for test targets")
    foreach(tgt gtest gtest_main gmock gmock_main)
      set_target_properties(${tgt} PROPERTIES SYSTEM TRUE)
    endforeach()
endif ()

# ---------------------------------------------------------------------------
# Google Benchmark  (v1.9.5 — micro-benchmark support)
# ---------------------------------------------------------------------------
# Suppress its own test suite to avoid a nested GoogleTest build that would
# conflict with the one above.
CPMAddPackage(
    URI "gh:google/benchmark@1.9.5"
    OPTIONS "BENCHMARK_ENABLE_TESTING OFF"
            "BENCHMARK_ENABLE_INSTALL OFF"
)

# Google Benchmark's own library must be compiled with optimisations for
# meaningful timing results, even when the parent project is Debug.
if(benchmark_ADDED)
  foreach(tgt benchmark benchmark_main)
    target_compile_options(${tgt} PRIVATE
          $<$<NOT:$<CONFIG:Release>>:-O2 -DNDEBUG>
        )
  endforeach()
endif()
