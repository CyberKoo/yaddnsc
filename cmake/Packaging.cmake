# Shared CPack packaging configuration for yaddnsc.
#
# Produces runtime and development packages from the "runtime" and "dev"
# install components. DEB and RPM use separate builds because their systemd
# unit directories can differ.

option(YADDNSC_ENABLE_DEB "Enable DEB package generation (CPack)" OFF)
option(YADDNSC_ENABLE_RPM "Enable RPM package generation (CPack)" OFF)

if (YADDNSC_ENABLE_DEB AND YADDNSC_ENABLE_RPM)
    message(FATAL_ERROR "YADDNSC_ENABLE_DEB and YADDNSC_ENABLE_RPM must be built separately")
endif ()

set(YADDNSC_LINUX_PACKAGING OFF)
if (YADDNSC_ENABLE_DEB OR YADDNSC_ENABLE_RPM)
    set(YADDNSC_LINUX_PACKAGING ON)
else ()
    return()
endif ()

# ---------------------------------------------------------------------------
# Package version — use git describe for dev builds
# ---------------------------------------------------------------------------
set(PACKAGE_VERSION "${PROJECT_VERSION}")

if (YADDNSC_DEVELOPMENT AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    include(GitVersion)
    if (GIT_ON_TAG AND GIT_PRERELEASE)
        set(PACKAGE_VERSION "${PROJECT_VERSION}~${GIT_PRERELEASE}")
    elseif (NOT GIT_ON_TAG)
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
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

# Glaze's install rules hardcode their "glaze_Development" component. Group it
# with the SDK's "dev" component so both generators produce one development
# package, keyed by the "devel" group name.
set(CPACK_COMPONENTS_GROUPING ONE_PER_GROUP)
set(CPACK_COMPONENTS_ALL runtime dev glaze_Development)
set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "yaddnsc runtime")
set(CPACK_COMPONENT_DEV_DISPLAY_NAME "yaddnsc plugin development files")
set(CPACK_COMPONENT_DEV_GROUP devel)
set(CPACK_COMPONENT_GLAZE_DEVELOPMENT_GROUP devel)
set(CPACK_COMPONENT_GLAZE_DEVELOPMENT_HIDDEN ON)
set(CPACK_DEVEL_PACKAGE_DESCRIPTION
        "Plugin development files for yaddnsc — C++ SDK headers, CMake package \
configuration, and header-only plugin crypto helpers.")

set(CPACK_GENERATOR "")

if (YADDNSC_ENABLE_DEB)
    include(PackagingDeb)
endif ()

if (YADDNSC_ENABLE_RPM)
    include(PackagingRpm)
endif ()

include(CPack)
