# ==============================================================================
# Main executable — main.cpp + the composition root (which pulls the whole
# module graph transitively)
# ==============================================================================
add_executable(yaddnsc src/main.cpp)
target_link_libraries(yaddnsc PRIVATE
    yaddnsc_warnings
    yaddnsc_build_options
    yaddnsc_internal_headers
    yaddnsc_composition
    yaddnsc_cli_adapter
    spdlog::spdlog
)

# Sanitizer flags for main.cpp and the link step
add_sanitizer_flags(yaddnsc)

# ---------------------------------------------------------------------------
# Linker options
# ---------------------------------------------------------------------------
# Strip symbols in Release builds
target_link_options(yaddnsc PRIVATE
        $<$<CONFIG:Release>:$<$<PLATFORM_ID:Darwin>:-Wl,-S>>
        $<$<CONFIG:Release>:$<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,-s>>
)

# ---------------------------------------------------------------------------
# Target properties
# ---------------------------------------------------------------------------
# LTO is incompatible with musl (auto-detected in FeatureCheck).
# LTO is limited to Release builds only:
#   - Debug (-O0):  no meaningful optimization, adds link-time overhead.
#   - Sanitizer (-O1): small optimization gain, but can interfere with
#     ASan backtraces and UBSan diagnostics.
if (NOT LIBC_MUSL)
  set_target_properties(yaddnsc PROPERTIES INTERPROCEDURAL_OPTIMIZATION
      $<$<CONFIG:Release>:TRUE>)
endif ()

# --export-dynamic is intentionally NOT set: driver plugins load through the
# C ABI alone and must not resolve any host symbols.

# Third-party link dependencies flow transitively from the module targets
# (composition pulls the full module graph); nothing extra is linked here.
