# ==============================================================================
# Sanitizer build type flags
# ==============================================================================
# Support for dedicated Sanitizer build type (used in CI for UBSan/ASan testing).
# Usage: cmake -B build -DCMAKE_BUILD_TYPE=Sanitizer
#
# These variables are defined here (rather than in CMakeLists.txt) because
# the feature-detection results below are needed to conditionally append
# optional flags such as -fsanitize=integer.
# ==============================================================================

set(CMAKE_CXX_FLAGS_SANITIZER
    "-O1 -ggdb -gdwarf-3 -fno-omit-frame-pointer"
    CACHE STRING "Flags used by the Sanitizer build type." FORCE)
set(CMAKE_C_FLAGS_SANITIZER
    "-O1 -ggdb -gdwarf-3 -fno-omit-frame-pointer"
    CACHE STRING "Flags used by the Sanitizer build type." FORCE)

# Portable core sanitizer flags (supported by both GCC and Clang).
string(APPEND CMAKE_CXX_FLAGS_SANITIZER " -fsanitize=address,undefined")
string(APPEND CMAKE_C_FLAGS_SANITIZER " -fsanitize=address,undefined")
string(APPEND CMAKE_EXE_LINKER_FLAGS_SANITIZER " -fsanitize=address,undefined")

# ==============================================================================
# Sanitizer feature detection
# ==============================================================================
# Individual sanitizer flags may be unsupported on some platforms (musl, ARM64,
# or custom GCC builds without the full UBSan suite).  Each flag is tested at
# configure time and the result is exported as a cache variable.
#
# Output variables:
#   HAVE_SANITIZE_INTEGER          — TRUE if the compiler supports -fsanitize=integer
#   HAVE_SANITIZE_BOUNDS           — TRUE if the compiler supports -fsanitize=bounds
#   HAVE_SANITIZE_NULL             — TRUE if the compiler supports -fsanitize=null
#   HAVE_SANITIZE_ALIGNMENT        — TRUE if the compiler supports -fsanitize=alignment
#   HAVE_ASAN_USE_AFTER_RETURN     — TRUE if the compiler supports
#                                    -fsanitize-address-use-after-return=always
# ==============================================================================

include(CheckCXXCompilerFlag)

check_cxx_compiler_flag("-fsanitize=integer"          HAVE_SANITIZE_INTEGER)
check_cxx_compiler_flag("-fsanitize=bounds"           HAVE_SANITIZE_BOUNDS)
check_cxx_compiler_flag("-fsanitize=null"             HAVE_SANITIZE_NULL)
check_cxx_compiler_flag("-fsanitize=alignment"        HAVE_SANITIZE_ALIGNMENT)
check_cxx_compiler_flag("-fsanitize-address-use-after-return=always"
                        HAVE_ASAN_USE_AFTER_RETURN)

foreach(flag IN ITEMS integer bounds null alignment)
  if(HAVE_SANITIZE_${flag})
    message(STATUS "Sanitizer flags: -fsanitize=${flag}  => yes")
  else()
    message(STATUS "Sanitizer flags: -fsanitize=${flag}  => no")
  endif()
endforeach()
if(HAVE_ASAN_USE_AFTER_RETURN)
  message(STATUS "Sanitizer flags: -fsanitize-address-use-after-return=always => yes")
else()
  message(STATUS "Sanitizer flags: -fsanitize-address-use-after-return=always => no")
endif()

# Integer sanitizer is optional — not supported on all platforms
if(HAVE_SANITIZE_INTEGER)
  string(APPEND CMAKE_CXX_FLAGS_SANITIZER " -fsanitize=integer")
  string(APPEND CMAKE_C_FLAGS_SANITIZER   " -fsanitize=integer")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_SANITIZER " -fsanitize=integer")
endif()

set(CMAKE_EXE_LINKER_FLAGS_SANITIZER    "${CMAKE_EXE_LINKER_FLAGS_SANITIZER}"    CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_SANITIZER "" CACHE STRING "" FORCE)

# ==============================================================================
# add_sanitizer_flags — apply sanitizer flags to a target
# ==============================================================================
# Usage: add_sanitizer_flags(yaddnsc)
#
# Applies compile and link sanitizer flags to the given target based on
# build configuration (Debug / Sanitizer) and configure-time feature
# detection.  Debug sanitizers are gated by the YADDNSC_SANITIZE_DEBUG
# option so coverage builds can opt out.
# ==============================================================================

function(add_sanitizer_flags TARGET)
  # Compile options: address + undefined (portable to all supported compilers)
  if(YADDNSC_SANITIZE_DEBUG)
    target_compile_options(${TARGET} PRIVATE
      $<$<CONFIG:Debug>:-fsanitize=address,undefined>
      $<$<CONFIG:Debug>:-fsanitize-address-use-after-scope>
    )
  endif()
  target_compile_options(${TARGET} PRIVATE
    $<$<CONFIG:Sanitizer>:-fsanitize=address,undefined>
    $<$<CONFIG:Sanitizer>:-fsanitize-address-use-after-scope>
  )

  # Compile options: Clang-only flags (bounds, null, alignment) — feature-detected
  foreach(san bounds null alignment)
    if(HAVE_SANITIZE_${san})
      target_compile_options(${TARGET} PRIVATE
        $<$<OR:$<CONFIG:Debug>,$<CONFIG:Sanitizer>>:-fsanitize=${san}>
      )
    endif()
  endforeach()

  # Compile options: integer (may be unsupported)
  if(HAVE_SANITIZE_INTEGER)
    target_compile_options(${TARGET} PRIVATE
      $<$<OR:$<CONFIG:Debug>,$<CONFIG:Sanitizer>>:-fsanitize=integer>
    )
  endif()

  # Compile options: use-after-return (may be unsupported)
  if(HAVE_ASAN_USE_AFTER_RETURN)
    target_compile_options(${TARGET} PRIVATE
      $<$<OR:$<CONFIG:Debug>,$<CONFIG:Sanitizer>>:-fsanitize-address-use-after-return=always>
    )
  endif()

  # Linker options: address + undefined (portable)
  if(YADDNSC_SANITIZE_DEBUG)
    target_link_options(${TARGET} PRIVATE
      $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
  endif()
  target_link_options(${TARGET} PRIVATE
    $<$<CONFIG:Sanitizer>:-fsanitize=address,undefined>
  )

  # Linker options: Clang-only flags (bounds, null, alignment) — feature-detected
  foreach(san bounds null alignment)
    if(HAVE_SANITIZE_${san})
      target_link_options(${TARGET} PRIVATE
        $<$<OR:$<CONFIG:Debug>,$<CONFIG:Sanitizer>>:-fsanitize=${san}>
      )
    endif()
  endforeach()

  # Linker options: integer (only if the compiler supports it)
  if(HAVE_SANITIZE_INTEGER)
    target_link_options(${TARGET} PRIVATE
      $<$<OR:$<CONFIG:Debug>,$<CONFIG:Sanitizer>>:-fsanitize=integer>
    )
  endif()
endfunction()
