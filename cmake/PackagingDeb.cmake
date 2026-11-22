# DEB-specific CPack configuration. Included by Packaging.cmake when enabled.

list(APPEND CPACK_GENERATOR DEB)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_CONTROLS_STRICT_PERMISSION ON)

set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "yaddnsc")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_RUNTIME_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "libstdc++6 (>= 14)")

# CMake 4.x no longer supports CPACK_DEBIAN_PACKAGE_CONFFILES. CPack copies each
# control file into the control archive using its original basename.
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
