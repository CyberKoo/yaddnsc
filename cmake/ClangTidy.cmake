# ==============================================================================
# Clang-Tidy (static analysis)
# ==============================================================================
# Find the clang-tidy executable on the system. Prefer the unversioned name
# (distro default); fall back to versioned binaries installed alongside a
# specific LLVM toolchain.
find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-21 clang-tidy-20 clang-tidy-19 clang-tidy-18)

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
    message(STATUS "Clang-Tidy not found — static analysis will be skipped.")
endif()
