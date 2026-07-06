# Feature detection: system symbols, library features, etc.

include(CheckSymbolExists)
include(CheckCXXSourceCompiles)
include(CheckStructHasMember)

# --- resolver feature checks ------------------------------------------------

check_symbol_exists(res_nquery "netinet/in.h;resolv.h" HAVE_RES_NQUERY)
check_symbol_exists(res_setservers "netinet/in.h;resolv.h" HAVE_RES_SETSERVERS)
check_symbol_exists(res_ndestroy "netinet/in.h;resolv.h" HAVE_RES_NDESTROY)
check_struct_has_member("struct __res_state" _u._ext.nsaddrs "netinet/in.h;resolv.h" HAVE_RES_STATE_EXT_NSADDRS LANGUAGE C)

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
