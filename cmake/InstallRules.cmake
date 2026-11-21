# Installation rules for yaddnsc.
#
# Configures install targets, config file placement, systemd service unit,
# and includes DEB/RPM packaging support.

include(Packaging)

# ==============================================================================
# Binary
# ==============================================================================
install(TARGETS yaddnsc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)

if (YADDNSC_ENABLE_RPM)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT runtime)
endif ()

# ==============================================================================
# Default configuration file
# ==============================================================================
# Native packages always use /etc/yaddnsc. DEB and RPM both preserve modified
# configuration files on upgrade through their package-specific metadata.
if (YADDNSC_LINUX_PACKAGING)
    install(FILES ${CMAKE_SOURCE_DIR}/template/deb/yaddnsc_config.json DESTINATION /etc/yaddnsc RENAME config.json COMPONENT runtime)
else ()
    install(FILES ${CMAKE_SOURCE_DIR}/template/deb/yaddnsc_config.json DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/yaddnsc RENAME config.json COMPONENT runtime)
endif ()

if (YADDNSC_LINUX_PACKAGING)
    set(YADDNSC_SYSCONF_DIR "/etc/yaddnsc")
else ()
    set(YADDNSC_SYSCONF_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/yaddnsc")
endif ()

# ==============================================================================
# systemd service unit
# ==============================================================================
# Native packages always include the unit. For other builds, install it when
# the systemd development files are detected.
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

if (YADDNSC_LINUX_PACKAGING AND NOT SYSTEMD_UNIT_DIR)
    if (YADDNSC_ENABLE_RPM)
        set(SYSTEMD_UNIT_DIR "/usr/lib/systemd/system")
    else ()
        set(SYSTEMD_UNIT_DIR "/lib/systemd/system")
    endif ()
    message(STATUS "Native packaging enabled — using default systemd unit dir: ${SYSTEMD_UNIT_DIR}")
endif ()

if (SYSTEMD_UNIT_DIR)
    # The unit embeds absolute binary paths. Native package contents follow the
    # CPack packaging prefix, so the unit must too.
    if (YADDNSC_LINUX_PACKAGING)
        set(YADDNSC_UNIT_BINDIR "${CPACK_PACKAGING_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
    else ()
        set(YADDNSC_UNIT_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}")
    endif ()
    configure_file(
            "${CMAKE_SOURCE_DIR}/template/deb/yaddnsc.service.in"
            "${CMAKE_BINARY_DIR}/generated/yaddnsc.service"
            @ONLY
    )
    install(FILES "${CMAKE_BINARY_DIR}/generated/yaddnsc.service" DESTINATION "${SYSTEMD_UNIT_DIR}" COMPONENT runtime)
endif ()

# ==============================================================================
# Shell completions (zsh, bash, fish)
# ==============================================================================
# Native packages use the vendor/site paths that are in the default completion
# search path for each shell.

# -- zsh --
if (YADDNSC_ENABLE_DEB)
    set(YADDNSC_ZSH_COMPLETION_DIR "share/zsh/vendor-completions")
else ()
    set(YADDNSC_ZSH_COMPLETION_DIR "share/zsh/site-functions")
endif ()

install(
    FILES ${CMAKE_SOURCE_DIR}/template/zsh/_yaddnsc
    DESTINATION ${YADDNSC_ZSH_COMPLETION_DIR}
    COMPONENT runtime
)

# -- bash --
# bash-completion (v2+) ships a default /usr/share/bash-completion/
# layout that is the standard on all major distros, so the same path
# works for both DEB and non-DEB builds.
install(
    FILES ${CMAKE_SOURCE_DIR}/template/bash/yaddnsc
    DESTINATION "share/bash-completion/completions"
    COMPONENT runtime
)

# -- fish --
if (YADDNSC_LINUX_PACKAGING)
    set(YADDNSC_FISH_COMPLETION_DIR "share/fish/vendor_completions.d")
else ()
    set(YADDNSC_FISH_COMPLETION_DIR "share/fish/completions")
endif ()

install(
    FILES ${CMAKE_SOURCE_DIR}/template/fish/yaddnsc.fish
    DESTINATION ${YADDNSC_FISH_COMPLETION_DIR}
    COMPONENT runtime
)

# ==============================================================================
# Plugin SDK — development files for third-party driver authors
# ==============================================================================
# Third-party drivers build without the host sources.  Export actual CMake
# targets instead of relying on source-tree-only interface libraries or a
# hand-assembled crypto source path.  All files in this section belong to the
# "dev" component, which CPack packs into the separate yaddnsc-dev package.
install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/include/yaddnsc/sdk
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/yaddnsc
    COMPONENT dev
)
install(FILES ${PROJECT_BINARY_DIR}/generated/sdk_include/yaddnsc/sdk/version.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/yaddnsc/sdk COMPONENT dev)
install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/include/yaddnsc/util
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/yaddnsc
    COMPONENT dev
)

install(
    TARGETS yaddnsc_plugin_abi
    EXPORT yaddnscAbiTargets
    COMPONENT dev
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    TARGETS yaddnsc_plugin_sdk
    EXPORT yaddnscSdkTargets
    COMPONENT dev
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    TARGETS yaddnsc_plugin_sdk_crypto
    EXPORT yaddnscCryptoTargets
    COMPONENT dev
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
if (YADDNSC_HAS_SDK_XML_COMPONENT)
    install(
        TARGETS yaddnsc_plugin_sdk_xml
        EXPORT yaddnscXmlTargets
        COMPONENT dev
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif ()

if (YADDNSC_LINUX_PACKAGING)
    set(YADDNSC_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/cmake/yaddnsc")
else ()
    set(YADDNSC_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/yaddnsc")
endif ()

install(
    EXPORT yaddnscAbiTargets
    FILE yaddnscAbiTargets.cmake
    NAMESPACE yaddnsc::
    DESTINATION ${YADDNSC_CMAKE_INSTALL_DIR}
    COMPONENT dev
)
install(
    EXPORT yaddnscSdkTargets
    FILE yaddnscSdkTargets.cmake
    NAMESPACE yaddnsc::
    DESTINATION ${YADDNSC_CMAKE_INSTALL_DIR}
    COMPONENT dev
)
install(
    EXPORT yaddnscCryptoTargets
    FILE yaddnscCryptoTargets.cmake
    NAMESPACE yaddnsc::
    DESTINATION ${YADDNSC_CMAKE_INSTALL_DIR}
    COMPONENT dev
)
if (YADDNSC_HAS_SDK_XML_COMPONENT)
    install(
        EXPORT yaddnscXmlTargets
        FILE yaddnscXmlTargets.cmake
        NAMESPACE yaddnsc::
        DESTINATION ${YADDNSC_CMAKE_INSTALL_DIR}
        COMPONENT dev
    )
endif ()

include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/template/cmake/yaddnscConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/yaddnscConfig.cmake
    INSTALL_DESTINATION ${YADDNSC_CMAKE_INSTALL_DIR}
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/yaddnscConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/yaddnscConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/yaddnscConfigVersion.cmake
    DESTINATION ${YADDNSC_CMAKE_INSTALL_DIR}
    COMPONENT dev
)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/test/sdk_consumer/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/yaddnsc/examples COMPONENT dev
    FILES_MATCHING PATTERN "*.cpp" PATTERN "*.c" PATTERN "CMakeLists.txt" PATTERN "README.md" PATTERN "verify.cmake")
