# ==============================================================================
# Clang-Tidy (static analysis)
# ==============================================================================
# Find the clang-tidy executable on the system
find_program(CLANG_TIDY_EXE NAMES "clang-tidy")

if(CLANG_TIDY_EXE)
    message(STATUS "Clang-Tidy found: ${CLANG_TIDY_EXE}")

    # Only run clang-tidy with Clang as the compiler to avoid parsing
    # incompatibilities between clang-tidy's clang frontend and GCC's
    # standard library headers (e.g. C++23 std::expected).
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    else()
        message(STATUS "Clang-Tidy skipped (compiler is ${CMAKE_CXX_COMPILER_ID}, not Clang)")
    endif()
else()
    message(WARNING "Clang-Tidy not found! Static analysis will be skipped.")
endif()
