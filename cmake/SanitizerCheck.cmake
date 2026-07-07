# ==============================================================================
# Sanitizer feature detection
# ==============================================================================
# Individual sanitizer flags may be unsupported on some platforms (musl, ARM64,
# or custom GCC builds without the full UBSan suite).  Each flag is tested at
# configure time and the result is exported as a cache variable.
#
# Output variables:
#   HAVE_SANITIZE_INTEGER          — TRUE if the compiler supports -fsanitize=integer
#   HAVE_ASAN_USE_AFTER_RETURN     — TRUE if the compiler supports
#                                    -fsanitize-address-use-after-return=always
# ==============================================================================

include(CheckCXXCompilerFlag)

check_cxx_compiler_flag("-fsanitize=integer"          HAVE_SANITIZE_INTEGER)
check_cxx_compiler_flag("-fsanitize-address-use-after-return=always"
                        HAVE_ASAN_USE_AFTER_RETURN)

if(HAVE_SANITIZE_INTEGER)
    message(STATUS "Sanitizer flags: -fsanitize=integer        => yes")
else()
    message(STATUS "Sanitizer flags: -fsanitize=integer        => no")
endif()
if(HAVE_ASAN_USE_AFTER_RETURN)
    message(STATUS "Sanitizer flags: -fsanitize-address-use-after-return=always => yes")
else()
    message(STATUS "Sanitizer flags: -fsanitize-address-use-after-return=always => no")
endif()
