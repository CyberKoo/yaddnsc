# Determine YADDNSC_RELEASE_INFO — the version suffix embedded in the binary.
#
# Input variable:
#   YADDNSC_DEVELOPMENT  — option() set in CMakeLists.txt; OFF forces RELEASE.
#
# Output variable:
#   YADDNSC_RELEASE_INFO — "RELEASE" or a git describe string (e.g. main~gabc1234).
#
# Strategy:
#   - In a git clone with a tag matching v${PROJECT_VERSION} or ${PROJECT_VERSION}
#     → RELEASE
#   - In a git clone on an untagged commit → git describe (e.g. main~gabc1234)
#   - In a source tarball (no .git) → RELEASE
#   - If Git is not installed but .git exists → RELEASE (fail-safe)

if (YADDNSC_DEVELOPMENT AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    include(GitVersion)
    if (GIT_ON_TAG)
        set(YADDNSC_RELEASE_INFO RELEASE)
    else ()
        set(YADDNSC_RELEASE_INFO ${GIT_DESCRIBE})
    endif ()
else ()
    set(YADDNSC_RELEASE_INFO RELEASE)
endif ()
