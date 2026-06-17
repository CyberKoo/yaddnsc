# Feature detection: system symbols, library features, etc.

include(CheckSymbolExists)
include(CheckCXXSourceCompiles)
include(CheckStructHasMember)

# --- resolver feature checks ------------------------------------------------

check_symbol_exists(res_nquery "netinet/in.h;resolv.h" HAVE_RES_NQUERY)
check_symbol_exists(res_setservers "netinet/in.h;resolv.h" HAVE_RES_SETSERVERS)
check_symbol_exists(res_ndestroy "netinet/in.h;resolv.h" HAVE_RES_NDESTROY)
check_struct_has_member("struct __res_state" _u._ext.nsaddrs "netinet/in.h;resolv.h" HAVE_RES_STATE_EXT_NSADDRS LANGUAGE C)

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

check_cxx_source_compiles("
    #ifndef __MUSL__
    #error not musl
    #endif
    int main() { return 0; }
" LIBC_MUSL)
