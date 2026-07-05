# Single source of truth for git version information.
#
# Output variables:
#   GIT_AVAILABLE      — TRUE if Git was found and git info was retrieved
#   GIT_ON_TAG         — TRUE if HEAD exactly matches v${PROJECT_VERSION} or ${PROJECT_VERSION}
#   GIT_DESCRIBE       — cleaned describe string (e.g. "main~gabc1234-dirty"), or "unknown"
#   GIT_DESCRIBE_CLEAN — same as GIT_DESCRIBE but with "-dirty" stripped
#   GIT_DIRTY          — TRUE if the working tree has uncommitted changes
#
# Backward-compatible alias:
#   GIT_VERSION        — same as GIT_DESCRIBE (deprecated, prefer GIT_DESCRIBE)

find_package(Git QUIET)

if (NOT Git_FOUND)
    set(GIT_AVAILABLE FALSE)
    set(GIT_ON_TAG FALSE)
    set(GIT_DESCRIBE "unknown")
    set(GIT_DESCRIBE_CLEAN "unknown")
    set(GIT_DIRTY FALSE)
    set(GIT_VERSION "unknown")
    return()
endif ()

# Check if HEAD is on an exact version tag
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} describe --exact-match --tags HEAD
        OUTPUT_VARIABLE GIT_EXACT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
if (GIT_EXACT_TAG MATCHES "^v?${PROJECT_VERSION}$")
    set(GIT_ON_TAG TRUE)
else ()
    set(GIT_ON_TAG FALSE)
endif ()

# Get describe string for dev builds — e.g. "heads/main-0-gabc1234-dirty"
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} describe --all --long --dirty
        OUTPUT_VARIABLE GIT_RAW
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

# Transform "heads/branch-0-gsha" → "branch~gsha"
string(REGEX REPLACE "^heads\\/(.*)-0-(g.*)$" "\\1~\\2" GIT_DESCRIBE "${GIT_RAW}")

# Determine dirty status
if (GIT_DESCRIBE MATCHES "-dirty$")
    set(GIT_DIRTY TRUE)
    string(REGEX REPLACE "-dirty$" "" GIT_DESCRIBE_CLEAN "${GIT_DESCRIBE}")
else ()
    set(GIT_DIRTY FALSE)
    set(GIT_DESCRIBE_CLEAN "${GIT_DESCRIBE}")
endif ()

# Backward-compatible alias
set(GIT_VERSION "${GIT_DESCRIBE}")
