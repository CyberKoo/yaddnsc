# Installation rules for yaddnsc.
#
# Configures install targets, config file placement, systemd service unit,
# and includes DEB/CPack packaging support.

# ==============================================================================
# Packaging — DEB via CPack
# ==============================================================================
# Included early so YADDNSC_ENABLE_DEB is available for the systemd logic below.
include(Packaging)

# ==============================================================================
# Binary
# ==============================================================================
install(TARGETS yaddnsc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ==============================================================================
# Default configuration file
# ==============================================================================
# For DEB packaging, install config to /etc/ so CPack auto-marks it as a
# conffile.  dpkg will then NEVER silently overwrite a user-modified conffile
# on upgrade — it prompts the user instead.
if(YADDNSC_ENABLE_DEB)
  # Absolute path — install to /etc/yaddnsc/ regardless of prefix.
  # CPack DEB generator auto-detects files under /etc/ as conffiles.
  install(FILES ${CMAKE_BINARY_DIR}/generated/config.json
          DESTINATION /etc/yaddnsc)
else()
  install(FILES ${CMAKE_BINARY_DIR}/generated/config.json
          DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/yaddnsc)
endif()

# System config directory for yaddnsc — this is the runtime path used by the
# service unit.  For DEB packaging it is always /etc/yaddnsc (Debian convention).
if(YADDNSC_ENABLE_DEB)
  set(YADDNSC_SYSCONF_DIR "/etc/yaddnsc")
else()
  set(YADDNSC_SYSCONF_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/yaddnsc")
endif()

# ==============================================================================
# systemd service unit
# ==============================================================================
# For DEB packaging, this is always installed since Debian/Ubuntu use systemd.
# For other builds, we try pkg-config first.
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  # Check for the libsystemd library (from libsystemd-dev / libsystemd.pc)
  pkg_check_modules(SYSTEMD libsystemd QUIET)
  if (SYSTEMD_FOUND)
    # Query the systemd module (from systemd-dev / systemd.pc) for the unit dir
    pkg_get_variable(SYSTEMD_UNIT_DIR systemd systemdsystemunitdir)
    message(STATUS "systemd detected via pkg-config — unit dir: ${SYSTEMD_UNIT_DIR}")
  endif ()
endif ()

if (YADDNSC_ENABLE_DEB AND NOT SYSTEMD_UNIT_DIR)
  set(SYSTEMD_UNIT_DIR "/lib/systemd/system")
  message(STATUS "DEB packaging enabled — using default systemd unit dir: ${SYSTEMD_UNIT_DIR}")
endif ()

if (SYSTEMD_UNIT_DIR)
  configure_file(
    "${CMAKE_SOURCE_DIR}/template/deb/yaddnsc.service.in"
    "${CMAKE_BINARY_DIR}/generated/yaddnsc.service"
    @ONLY
  )
  install(FILES "${CMAKE_BINARY_DIR}/generated/yaddnsc.service"
          DESTINATION "${SYSTEMD_UNIT_DIR}")
endif ()
