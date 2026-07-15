# Determine YADDNSC_RELEASE_INFO — the version suffix embedded in the binary.
#
# Input variable:
#   YADDNSC_DEVELOPMENT  — option() set in CMakeLists.txt; OFF forces RELEASE.
#
# Output variable:
#   YADDNSC_RELEASE_INFO — one of:
#     "RELEASE"          — on a tagged release (e.g. v1.0.0)
#     "alpha.1" etc.     — on a pre-release tag (e.g. v1.0.0-alpha.1)
#     git describe str   — on an untagged development commit
#
# Strategy:
#   - In a git clone with a tag matching v${PROJECT_VERSION} or ${PROJECT_VERSION}
#     → RELEASE
#   - In a git clone with a pre-release tag matching v${PROJECT_VERSION}-<suffix>
#     → <suffix> (e.g. alpha.1)
#   - In a git clone on an untagged commit → git describe (e.g. main~gabc1234)
#   - In a source tarball (no .git) → RELEASE
#   - If Git is not installed but .git exists → RELEASE (fail-safe)

if (YADDNSC_DEVELOPMENT AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    include(GitVersion)
    if (GIT_ON_TAG)
        if (GIT_PRERELEASE)
            set(YADDNSC_RELEASE_INFO ${GIT_PRERELEASE})
        else ()
            set(YADDNSC_RELEASE_INFO RELEASE)
        endif ()
    else ()
        set(YADDNSC_RELEASE_INFO ${GIT_DESCRIBE})
    endif ()
else ()
    set(YADDNSC_RELEASE_INFO RELEASE)
endif ()
