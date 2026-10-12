# CPack packaging configuration for yaddnsc.
#
# Provides DEB (and future RPM) packaging support.
# Controlled by the YADDNSC_ENABLE_DEB option.
#
# Produces two DEB packages from the "runtime" and "dev" install components:
# yaddnsc (the client itself) and yaddnsc-dev (plugin development files).
#
# On a tagged release, the package version matches PROJECT_VERSION.
# On a pre-release tag, the pre-release suffix is appended (e.g. 1.0.0~alpha.1).
# On a development commit, git describe info is appended (e.g. 1.0.0~gabc1234).

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
    if (GIT_ON_TAG AND GIT_PRERELEASE)
        # Pre-release tag — append suffix for correct ordering (e.g. 1.0.0~alpha.1)
        set(PACKAGE_VERSION "${PROJECT_VERSION}~${GIT_PRERELEASE}")
    elseif (NOT GIT_ON_TAG)
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
# Components — the install tree splits into two DEB packages:
#   runtime → yaddnsc      (binary, drivers, config, systemd unit, completions)
#   devel   → yaddnsc-dev  (plugin SDK headers, CMake package config, static
#                           plugin support library, bundled Glaze metadata)
# "devel" is a component GROUP (the name must not clash with a component):
# Glaze's own install rules hardcode their "glaze_Development" component, so
# it joins the project's "dev" component in the same package via grouping
# (CPACK_COMPONENTS_GROUPING defaults to ONE_PER_GROUP, and the DEB generator
# keys its per-package CPACK_DEBIAN_<NAME>_* variables off the grouping name).
# ---------------------------------------------------------------------------
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL runtime dev glaze_Development)

set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "yaddnsc runtime")
set(CPACK_COMPONENT_DEV_DISPLAY_NAME "yaddnsc plugin development files")
set(CPACK_COMPONENT_DEV_GROUP devel)
set(CPACK_COMPONENT_DEV_DEPENDS runtime)
set(CPACK_COMPONENT_GLAZE_DEVELOPMENT_GROUP devel)
set(CPACK_COMPONENT_GLAZE_DEVELOPMENT_HIDDEN ON)

set(CPACK_DEBIAN_DEVEL_DESCRIPTION
        "Plugin development files for yaddnsc — C++ SDK headers, CMake package \
configuration, and the plugin crypto support library.")

# ---------------------------------------------------------------------------
# DEB-specific settings
# ---------------------------------------------------------------------------
# Per-component CPACK_DEBIAN_<COMPONENT>_* variables override the generic
# CPACK_DEBIAN_PACKAGE_* defaults for that component's package.
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_CONTROLS_STRICT_PERMISSION ON)

# -- runtime package: yaddnsc --
set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "yaddnsc")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SHLIBDEPS ON)

# Minimum libstdc++6 version required.
# When built with GCC 14+, the auto-detected shlibs dependency may already
# capture this, but pinning it explicitly ensures installability on older
# systems that ship the runtime via updated libstdc++6 packages.
set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "libstdc++6 (>= 14)")

# DEB conffiles — mark /etc/yaddnsc/config.json so dpkg never silently
# overwrites a user-modified config on upgrade/reinstall.
# We ship the conffiles list via the CONTROL_EXTRA variable because
# CMake 4.x (Ubuntu 26.04) no longer supports CPACK_DEBIAN_PACKAGE_CONFFILES.
# The maintainer scripts manage the systemd unit, so like conffiles they
# belong to the runtime package only.
# CPack copies each extra file into the control archive using its original
# basename, so the generated file must be named "conffiles".
set(CPACK_DEBIAN_RUNTIME_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/template/deb/conffiles"
        "${CMAKE_SOURCE_DIR}/template/deb/postinst"
        "${CMAKE_SOURCE_DIR}/template/deb/postrm"
        "${CMAKE_SOURCE_DIR}/template/deb/prerm"
)

# -- development package: yaddnsc-dev (the "devel" component group) --
set(CPACK_DEBIAN_DEVEL_PACKAGE_NAME "yaddnsc-dev")
set(CPACK_DEBIAN_DEVEL_PACKAGE_SECTION "libdevel")

# Exact-version dependency on the matching runtime package.  The crypto SDK
# calls find_dependency(OpenSSL 3.0), so pull in libssl-dev as well.
set(CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS "yaddnsc (= ${PACKAGE_VERSION}), libssl-dev")
if (YADDNSC_HAS_SDK_XML_COMPONENT)
    # The plugin_sdk_xml component calls find_dependency(LibXml2).
    string(APPEND CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS ", libxml2-dev")
endif ()

# ---------------------------------------------------------------------------
# Generator — currently only DEB
# ---------------------------------------------------------------------------
set(CPACK_GENERATOR "DEB")

include(CPack)
