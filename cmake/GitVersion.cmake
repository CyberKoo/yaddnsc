find_package(Git QUIET)
if (GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} describe --all --long --dirty
            OUTPUT_VARIABLE "GIT_VERSION"
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    string(REGEX REPLACE "^([^ ]*/)?([^/]*)-0-(g.*)$" "\\2~\\3" GIT_VERSION "${GIT_VERSION}")
else ()
    set(GIT_VERSION unknown)
endif ()
