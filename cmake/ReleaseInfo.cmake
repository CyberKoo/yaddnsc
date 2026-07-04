# Determine YADDNSC_RELEASE_INFO — the version suffix embedded in the binary.
#
# Input variable:
#   YADDNSC_DEVELOPMENT  — option() set in CMakeLists.txt; OFF forces RELEASE.
#
# Output variable:
#   YADDNSC_RELEASE_INFO — either a git describe string (dev) or "RELEASE".
#
# Strategy:
#   - In a git clone with a tag matching v${PROJECT_VERSION} or ${PROJECT_VERSION}
#     → RELEASE
#   - In a git clone on an untagged commit → git describe (e.g. main~gabc1234)
#   - In a source tarball (no .git) → RELEASE
#   - If Git is not installed but .git exists → RELEASE (fail-safe)

if (YADDNSC_DEVELOPMENT AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  find_package(Git QUIET)
  if (Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --exact-match --tags HEAD
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if (GIT_TAG MATCHES "^v?${PROJECT_VERSION}$")
      set(YADDNSC_RELEASE_INFO RELEASE)
    else ()
      include(GitVersion)
      set(YADDNSC_RELEASE_INFO ${GIT_VERSION})
    endif ()
  else ()
    set(YADDNSC_RELEASE_INFO RELEASE)
  endif ()
else ()
  set(YADDNSC_RELEASE_INFO RELEASE)
endif ()
