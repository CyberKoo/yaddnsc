# CPack packaging configuration for yaddnsc.
#
# Provides DEB (and future RPM) packaging support.
# Controlled by the YADDNSC_ENABLE_DEB option.
#
# On a tagged release, the package version matches PROJECT_VERSION.
# On a development commit, git describe info is appended
# so the package is clearly distinguishable (e.g. 1.0.0~gabc1234).

option(YADDNSC_ENABLE_DEB "Enable DEB package generation (CPack)" OFF)

if (NOT YADDNSC_ENABLE_DEB)
    return()
endif ()

# ---------------------------------------------------------------------------
# Package version — use git describe for dev builds
# ---------------------------------------------------------------------------
set(PACKAGE_VERSION "${PROJECT_VERSION}")

if (YADDNSC_DEVELOPMENT AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    include(GitVersion)
    if (NOT GIT_ON_TAG)
        # Strip the "-dirty" suffix — inside a Docker build the working tree is
        # always "dirty" due to file metadata changes from COPY, which is meaningless.
        set(PACKAGE_VERSION "${PROJECT_VERSION}~${GIT_DESCRIBE_CLEAN}")
    endif ()
endif ()

# ---------------------------------------------------------------------------
# Core CPack metadata
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PACKAGE_VERSION}")
set(CPACK_PACKAGE_CONTACT "CyberKoo <2918558+CyberKoo@users.noreply.github.com>")
set(CPACK_PACKAGE_VENDOR "CyberKoo")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Yet Another Dynamic DNS Client — multi-driver DDNS updater")
set(CPACK_PACKAGE_DESCRIPTION
        "yaddnsc is a dynamic DNS (DDNS) update client with a modular driver \
architecture. It supports multiple DNS providers via loadable driver modules, \
multiple IP source detection methods, and concurrent DNS resolution.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/CyberKoo/yaddnsc")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

# Install prefix for CPack — /usr is standard for Linux distro packages
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

# ---------------------------------------------------------------------------
# DEB-specific settings
# ---------------------------------------------------------------------------
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_CONTROLS_STRICT_PERMISSION ON)

# Minimum libstdc++6 version required.
# When built with GCC 14+, the auto-detected shlibs dependency may already
# capture this, but pinning it explicitly ensures installability on older
# systems that ship the runtime via updated libstdc++6 packages.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libstdc++6 (>= 14)")

# ---------------------------------------------------------------------------
# DEB conffiles — mark /etc/yaddnsc/config.json so dpkg never silently
# overwrites a user-modified config on upgrade/reinstall.
# We ship the conffiles list via CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA because
# CMake 4.x (Ubuntu 26.04) no longer supports CPACK_DEBIAN_PACKAGE_CONFFILES.
# ---------------------------------------------------------------------------
# CPack copies the extra file into the control archive using its original
# basename, so the generated file must be named "conffiles".
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/template/deb/conffiles"
        "${CMAKE_SOURCE_DIR}/template/deb/postinst"
        "${CMAKE_SOURCE_DIR}/template/deb/postrm"
        "${CMAKE_SOURCE_DIR}/template/deb/prerm"
)

# ---------------------------------------------------------------------------
# Generator — currently only DEB
# ---------------------------------------------------------------------------
set(CPACK_GENERATOR "DEB")

include(CPack)
