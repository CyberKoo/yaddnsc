# CPack packaging configuration for yaddnsc.
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

# ---------------------------------------------------------------------------
# DEB
# ---------------------------------------------------------------------------
if (YADDNSC_ENABLE_DEB)
    list(APPEND CPACK_GENERATOR DEB)
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    set(CPACK_DEBIAN_PACKAGE_CONTROLS_STRICT_PERMISSION ON)

    set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "yaddnsc")
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "libstdc++6 (>= 14)")

    # CMake 4.x no longer supports CPACK_DEBIAN_PACKAGE_CONFFILES. CPack copies
    # each control file into the control archive using its original basename.
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_CONTROL_EXTRA
            "${CMAKE_SOURCE_DIR}/template/deb/conffiles"
            "${CMAKE_SOURCE_DIR}/template/deb/postinst"
            "${CMAKE_SOURCE_DIR}/template/deb/postrm"
            "${CMAKE_SOURCE_DIR}/template/deb/prerm"
    )

    set(CPACK_DEBIAN_DEVEL_PACKAGE_NAME "yaddnsc-dev")
    set(CPACK_DEBIAN_DEVEL_PACKAGE_SECTION "libdevel")
    set(CPACK_DEBIAN_DEVEL_DESCRIPTION "${CPACK_DEVEL_PACKAGE_DESCRIPTION}")
    set(CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS "libssl-dev (>= 3.0)")
    if (YADDNSC_HAS_SDK_XML_COMPONENT)
        string(APPEND CPACK_DEBIAN_DEVEL_PACKAGE_DEPENDS ", libxml2-dev")
    endif ()
endif ()

# ---------------------------------------------------------------------------
# RPM
# ---------------------------------------------------------------------------
if (YADDNSC_ENABLE_RPM)
    list(APPEND CPACK_GENERATOR RPM)
    set(YADDNSC_RPM_PACKAGE_RELEASE "1" CACHE STRING "RPM package release, independent of the project version")
    set(CPACK_RPM_COMPONENT_INSTALL ON)
    set(CPACK_RPM_MAIN_COMPONENT runtime)
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
    set(CPACK_RPM_PACKAGE_RELEASE "${YADDNSC_RPM_PACKAGE_RELEASE}")
    set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)
    set(CPACK_PACKAGE_RELOCATABLE OFF)
    set(CPACK_RPM_PACKAGE_RELOCATABLE OFF)

    set(CPACK_RPM_RUNTIME_PACKAGE_NAME "yaddnsc")
    set(CPACK_RPM_RUNTIME_PACKAGE_SUMMARY "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
    set(CPACK_RPM_RUNTIME_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
    set(CPACK_RPM_RUNTIME_PACKAGE_REQUIRES "systemd")
    set(CPACK_RPM_RUNTIME_FILE_NAME "RPM-DEFAULT")
    set(CPACK_RPM_RUNTIME_USER_FILELIST
            "%config(noreplace) /etc/yaddnsc/config.json"
            "%license ${CPACK_PACKAGING_INSTALL_PREFIX}/${CMAKE_INSTALL_DOCDIR}/LICENSE"
    )
    set(CPACK_RPM_RUNTIME_POST_INSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/template/rpm/postinst")
    set(CPACK_RPM_RUNTIME_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/template/rpm/prerm")
    set(CPACK_RPM_RUNTIME_POST_UNINSTALL_SCRIPT_FILE "${CMAKE_SOURCE_DIR}/template/rpm/postrm")

    set(CPACK_RPM_DEVEL_PACKAGE_NAME "yaddnsc-devel")
    set(CPACK_RPM_DEVEL_PACKAGE_SUMMARY "Plugin development files for yaddnsc")
    set(CPACK_RPM_DEVEL_PACKAGE_DESCRIPTION "${CPACK_DEVEL_PACKAGE_DESCRIPTION}")
    set(YADDNSC_RPM_OPENSSL_DEVEL_DEPENDENCY "" CACHE STRING
            "RPM OpenSSL development dependency (empty selects the distribution default)")
    if (NOT YADDNSC_RPM_OPENSSL_DEVEL_DEPENDENCY)
        set(YADDNSC_RPM_OPENSSL_DEVEL_DEPENDENCY "pkgconfig(openssl) >= 3.0")
        if (EXISTS "/etc/os-release")
            file(STRINGS "/etc/os-release" YADDNSC_OS_RELEASE_IDS REGEX "^(ID|ID_LIKE)=")
            if ("${YADDNSC_OS_RELEASE_IDS}" MATCHES "opensuse|sles|suse")
                set(YADDNSC_RPM_OPENSSL_DEVEL_DEPENDENCY "libopenssl-devel >= 3.0")
            endif ()
        endif ()
    endif ()
    set(CPACK_RPM_DEVEL_PACKAGE_REQUIRES "${YADDNSC_RPM_OPENSSL_DEVEL_DEPENDENCY}")
    set(CPACK_RPM_DEVEL_FILE_NAME "RPM-DEFAULT")
    if (YADDNSC_HAS_SDK_XML_COMPONENT)
        string(APPEND CPACK_RPM_DEVEL_PACKAGE_REQUIRES ", pkgconfig(libxml-2.0)")
    endif ()
endif ()

include(CPack)
