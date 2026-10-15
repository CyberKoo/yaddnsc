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
# ThreadSanitizer build type flags
# ==============================================================================
# Dedicated build type for data-race detection (used in CI). TSan cannot be
# combined with ASan/UBSan, so it is a separate build type rather than an
# extension of the Sanitizer configuration.
# Usage: cmake -B build -DCMAKE_BUILD_TYPE=ThreadSanitizer
# ==============================================================================

set(CMAKE_CXX_FLAGS_THREADSANITIZER
    "-O1 -ggdb -gdwarf-3 -fno-omit-frame-pointer"
    CACHE STRING "Flags used by the ThreadSanitizer build type." FORCE)
set(CMAKE_C_FLAGS_THREADSANITIZER
    "-O1 -ggdb -gdwarf-3 -fno-omit-frame-pointer"
    CACHE STRING "Flags used by the ThreadSanitizer build type." FORCE)

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
#   HAVE_SANITIZE_THREAD           — TRUE if the compiler supports -fsanitize=thread
#   HAVE_ASAN_USE_AFTER_RETURN     — TRUE if the compiler supports
#                                    -fsanitize-address-use-after-return=always
# ==============================================================================

include(CheckCXXCompilerFlag)

check_cxx_compiler_flag("-fsanitize=integer"          HAVE_SANITIZE_INTEGER)
check_cxx_compiler_flag("-fsanitize=bounds"           HAVE_SANITIZE_BOUNDS)
check_cxx_compiler_flag("-fsanitize=null"             HAVE_SANITIZE_NULL)
check_cxx_compiler_flag("-fsanitize=alignment"        HAVE_SANITIZE_ALIGNMENT)
# TSan emits a runtime registration constructor into every instrumented
# translation unit, so even a trivial probe program needs the runtime at
# link time.  check_cxx_compiler_flag applies the flag to the compile step
# only; add it to the probe link step too, or the result is a false
# negative (undefined reference to __tsan_init).
set(CMAKE_REQUIRED_LINK_OPTIONS_SAVED "${CMAKE_REQUIRED_LINK_OPTIONS}")
list(APPEND CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=thread")
check_cxx_compiler_flag("-fsanitize=thread"           HAVE_SANITIZE_THREAD)
set(CMAKE_REQUIRED_LINK_OPTIONS "${CMAKE_REQUIRED_LINK_OPTIONS_SAVED}")
unset(CMAKE_REQUIRED_LINK_OPTIONS_SAVED)
check_cxx_compiler_flag("-fsanitize-address-use-after-return=always"
                        HAVE_ASAN_USE_AFTER_RETURN)

foreach(flag IN ITEMS integer bounds null alignment)
  # check_cxx_compiler_flag() defines UPPERCASE cache variables; CMake variable
  # names are case-sensitive, so normalize into a separate variable and keep
  # the lowercase name for the flag itself.
  string(TOUPPER "${flag}" flag_probe)
  if(HAVE_SANITIZE_${flag_probe})
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
if(HAVE_SANITIZE_THREAD)
  message(STATUS "Sanitizer flags: -fsanitize=thread => yes")
else()
  message(STATUS "Sanitizer flags: -fsanitize=thread => no")
endif()

# Integer sanitizer is optional — not supported on all platforms
if(HAVE_SANITIZE_INTEGER)
  string(APPEND CMAKE_CXX_FLAGS_SANITIZER " -fsanitize=integer")
  string(APPEND CMAKE_C_FLAGS_SANITIZER   " -fsanitize=integer")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_SANITIZER " -fsanitize=integer")
endif()

# ThreadSanitizer build type — thread only, mutually exclusive with the
# address/undefined combination above (e.g. unsupported on musl targets).
if(HAVE_SANITIZE_THREAD)
  string(APPEND CMAKE_CXX_FLAGS_THREADSANITIZER " -fsanitize=thread")
  string(APPEND CMAKE_C_FLAGS_THREADSANITIZER   " -fsanitize=thread")
  string(APPEND CMAKE_EXE_LINKER_FLAGS_THREADSANITIZER " -fsanitize=thread")
endif()

set(CMAKE_EXE_LINKER_FLAGS_SANITIZER    "${CMAKE_EXE_LINKER_FLAGS_SANITIZER}"    CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_SANITIZER "" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS_THREADSANITIZER    "${CMAKE_EXE_LINKER_FLAGS_THREADSANITIZER}"    CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_THREADSANITIZER "" CACHE STRING "" FORCE)

# ==============================================================================
# yaddnsc_sanitizers — sanitizer flags as a linkable interface target
# ==============================================================================
# Carries compile and link sanitizer flags based on build configuration
# (Debug / Sanitizer) and the configure-time feature detection above.
# Link it through add_sanitizer_flags(<target>).
# ==============================================================================

add_library(yaddnsc_sanitizers INTERFACE)

# ------------------------------------------------------------------------
# Debug builds: address + undefined only (guidelines).
# This is the fast, low-false-positive combination used during development.
# Gated by YADDNSC_SANITIZE_DEBUG so coverage builds can opt out.
# NOTE: YADDNSC_SANITIZE_DEBUG must be defined before this file is included.
# ------------------------------------------------------------------------
if(YADDNSC_SANITIZE_DEBUG)
  target_compile_options(yaddnsc_sanitizers INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  )
  target_link_options(yaddnsc_sanitizers INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
  )
endif()

# ------------------------------------------------------------------------
# Sanitizer build type (CI): full combination.
# address,undefined plus integer,bounds,null,alignment and the aggressive
# use-after-return / use-after-scope modes. This is extremely expensive and
# triggers many false positives against STL internals, so it runs only as a
# dedicated CI sanitizer job (e.g. -DCMAKE_BUILD_TYPE=Sanitizer) for periodic
# deep testing — never on developer Debug builds.
# ------------------------------------------------------------------------
target_compile_options(yaddnsc_sanitizers INTERFACE
  $<$<CONFIG:Sanitizer>:-fsanitize=address,undefined>
  $<$<CONFIG:Sanitizer>:-fsanitize-address-use-after-scope>
)
target_link_options(yaddnsc_sanitizers INTERFACE
  $<$<CONFIG:Sanitizer>:-fsanitize=address,undefined>
)

# ------------------------------------------------------------------------
# ThreadSanitizer build type (CI): data-race detection. Mutually exclusive
# with the address/undefined combination, hence a separate build type.
# ------------------------------------------------------------------------
if(HAVE_SANITIZE_THREAD)
  target_compile_options(yaddnsc_sanitizers INTERFACE
    $<$<CONFIG:ThreadSanitizer>:-fsanitize=thread>
  )
  target_link_options(yaddnsc_sanitizers INTERFACE
    $<$<CONFIG:ThreadSanitizer>:-fsanitize=thread>
  )
endif()

# Compile options: optional flags (bounds, null, alignment) — feature-detected,
# Sanitizer build type only.
foreach(san bounds null alignment)
  string(TOUPPER "${san}" san_probe)
  if(HAVE_SANITIZE_${san_probe})
    target_compile_options(yaddnsc_sanitizers INTERFACE
      $<$<CONFIG:Sanitizer>:-fsanitize=${san}>
    )
  endif()
endforeach()

# Compile options: integer (may be unsupported), Sanitizer build type only.
if(HAVE_SANITIZE_INTEGER)
  target_compile_options(yaddnsc_sanitizers INTERFACE
    $<$<CONFIG:Sanitizer>:-fsanitize=integer>
  )
endif()

# Compile options: use-after-return (may be unsupported), Sanitizer build type only.
if(HAVE_ASAN_USE_AFTER_RETURN)
  target_compile_options(yaddnsc_sanitizers INTERFACE
    $<$<CONFIG:Sanitizer>:-fsanitize-address-use-after-return=always>
  )
endif()

# Linker options: optional flags (bounds, null, alignment) — feature-detected,
# Sanitizer build type only.
foreach(san bounds null alignment)
  string(TOUPPER "${san}" san_probe)
  if(HAVE_SANITIZE_${san_probe})
    target_link_options(yaddnsc_sanitizers INTERFACE
      $<$<CONFIG:Sanitizer>:-fsanitize=${san}>
    )
  endif()
endforeach()

# Linker options: integer (only if the compiler supports it), Sanitizer build type only.
if(HAVE_SANITIZE_INTEGER)
  target_link_options(yaddnsc_sanitizers INTERFACE
    $<$<CONFIG:Sanitizer>:-fsanitize=integer>
  )
endif()

# ==============================================================================
# add_sanitizer_flags — apply sanitizer flags to a target
# ==============================================================================
# Usage: add_sanitizer_flags(yaddnsc)
# ==============================================================================

function(add_sanitizer_flags TARGET)
  # Sanitizer instrumentation is a property of this source build.  Do not
  # export the internal flags target through installable static libraries:
  # downstream plugins choose their own instrumentation.
  target_link_libraries(${TARGET} PRIVATE $<BUILD_INTERFACE:yaddnsc_sanitizers>)
endfunction()
