cmake_minimum_required(VERSION 3.22...3.31 FATAL_ERROR)

# ==============================================================================
# Driver modules
# ==============================================================================
add_custom_target(drivers)

# The set of bundled drivers is an explicit, auditable list — no directory
# globbing. Runtime auto-discovery (skipping foreign libraries in the driver
# directory) is unaffected by this build-time list.
set(YADDNSC_DRIVERS
    alibaba_cloud
    cloudflare
    digital_ocean
    dnspod
    duckdns
    godaddy
    linode
    namecheap
    porkbun
    route53
    simple
    vultr
)

foreach (DRIVER_NAME ${YADDNSC_DRIVERS})
    message(STATUS "Build driver ${DRIVER_NAME}")

    add_subdirectory(${DRIVER_NAME})

    # Skip configuration if the subdirectory did not create a target
    # (e.g. optional dependency not found).
    if (TARGET ${DRIVER_NAME})

      # Output filename is <name>.so (no lib prefix) so the driver loader can
      # find it by just <name>.  The CMake target is also named <name>.
      set_target_properties(${DRIVER_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${DRIVER_NAME}")

      # -----------------------------------------------------------------------
      # Drivers are self-contained plugins: they link only the SDK (per-driver
      # CMakeLists) and never see host-internal headers or compile config.
      # -----------------------------------------------------------------------

      # Driver-specific extra compile options
      target_compile_options(${DRIVER_NAME} PRIVATE -Wnon-virtual-dtor)

      # Sanitizer instrumentation (must match the main executable's flags)
      add_sanitizer_flags(${DRIVER_NAME})

      # -----------------------------------------------------------------------
      # Linker options
      # -----------------------------------------------------------------------
      # Strip symbols in Release builds
      target_link_options(${DRIVER_NAME} PRIVATE
                  $<$<CONFIG:Release>:$<$<PLATFORM_ID:Darwin>:-Wl,-S>>
                  $<$<CONFIG:Release>:$<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,-s>>
              )

      # -----------------------------------------------------------------------
      # Target properties
      # -----------------------------------------------------------------------
      # LTO is incompatible with musl (auto-detected in root FeatureCheck)
      if (NOT LIBC_MUSL)
        set_target_properties(${DRIVER_NAME} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
      endif ()

      # -----------------------------------------------------------------------
      # Aggregate all drivers under the top-level 'drivers' target
      # -----------------------------------------------------------------------
      add_dependencies(drivers ${DRIVER_NAME})

      # -----------------------------------------------------------------------
      # Install rules
      # -----------------------------------------------------------------------
      install(TARGETS ${DRIVER_NAME} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/yaddnsc/drivers)

    endif ()
endforeach ()
