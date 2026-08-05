# Feature detection: system symbols, library features, etc.

include(CheckSymbolExists)
include(CheckCXXSourceCompiles)

# --- socket feature checks -------------------------------------------------

check_symbol_exists(MSG_NOSIGNAL "sys/socket.h" HAVE_MSG_NOSIGNAL)

# --- std::format availability ------------------------------------------------

check_cxx_source_compiles(
    "
    #include <format>
    #include <string>
    int main() {
        auto s = std::format(\"{}\", 42);
        (void)s;
        return 0;
    }
    "
    HAVE_STD_FORMAT
)
set(HAVE_STD_FORMAT "${HAVE_STD_FORMAT}" CACHE INTERNAL "Compiler supports std::format")

# --- libc detection (musl vs glibc) -------------------------------------------
# LTO is incompatible with musl, so we need to know which libc is in use.

message(STATUS "Looking for musl")
execute_process(
    COMMAND sh -c "ldd --version 2>&1"
    OUTPUT_VARIABLE LDD_OUTPUT
    ERROR_QUIET
)

if(LDD_OUTPUT MATCHES "musl")
  set(LIBC_MUSL 1)
  message(STATUS "Looking for musl - found")
else()
  set(LIBC_MUSL 0)
  message(STATUS "Looking for musl - not found")
endif()
