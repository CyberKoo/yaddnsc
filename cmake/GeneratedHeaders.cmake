# Generated headers (all configure_file calls, after all variables are defined)
# ==============================================================================
configure_file("${CMAKE_SOURCE_DIR}/template/headers/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/template/headers/build_id.hpp.in" "${CMAKE_BINARY_DIR}/generated/build_id.hpp" @ONLY)

# Default driver directory baked into the binary. DEB contents follow the
# CPack packaging prefix, so the default must too — CMAKE_INSTALL_FULL_LIBDIR
# would silently diverge when the configure-time prefix differs from it (same
# treatment as the systemd unit paths in InstallRules.cmake).
if (YADDNSC_ENABLE_DEB)
    set(YADDNSC_DEFAULT_DRIVER_DIR "${CPACK_PACKAGING_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/yaddnsc/drivers")
else ()
    set(YADDNSC_DEFAULT_DRIVER_DIR "${CMAKE_INSTALL_FULL_LIBDIR}/yaddnsc/drivers")
endif ()
configure_file("${CMAKE_SOURCE_DIR}/template/headers/config_cmake.h.in" "${CMAKE_BINARY_DIR}/generated/config_cmake.h" @ONLY)

configure_file("${CMAKE_SOURCE_DIR}/template/headers/logging_pattern.h.in" "${CMAKE_BINARY_DIR}/generated/logging_pattern.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/template/headers/min_update_interval.h.in" "${CMAKE_BINARY_DIR}/generated/min_update_interval.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/template/headers/resolver_config.h.in" "${CMAKE_BINARY_DIR}/generated/resolver_config.h" @ONLY)
