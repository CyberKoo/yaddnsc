# ==============================================================================
# Documentation — Doxygen
# ==============================================================================
option(YADDNSC_BUILD_DOCS "Build Doxygen API documentation" OFF)

if (YADDNSC_BUILD_DOCS)
  find_package(Doxygen REQUIRED)

  set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/docs)
  set(DOXYGEN_GENERATE_HTML YES)
  set(DOXYGEN_GENERATE_LATEX NO)
  set(DOXYGEN_USE_MDFILE_AS_MAINPAGE ${PROJECT_SOURCE_DIR}/README.md)
  set(DOXYGEN_STRIP_FROM_PATH ${PROJECT_SOURCE_DIR})
  set(DOXYGEN_EXTRACT_ALL NO)
  set(DOXYGEN_SOURCE_BROWSER YES)
  set(DOXYGEN_HTML_DYNAMIC_SECTIONS YES)
  set(DOXYGEN_DOT_IMAGE_FORMAT svg)
  set(DOXYGEN_INTERACTIVE_SVG YES)
  set(DOXYGEN_COLLABORATION_GRAPH YES)
  set(DOXYGEN_INCLUDE_GRAPH YES)
  set(DOXYGEN_INCLUDED_BY_GRAPH YES)
  set(DOXYGEN_CALL_GRAPH NO)
  set(DOXYGEN_CALLER_GRAPH NO)
  # Generate graphviz diagrams only if dot is available
  find_program(DOXYGEN_DOT_PATH dot)
  if (DOXYGEN_DOT_PATH)
    set(DOXYGEN_HAVE_DOT YES)
    message(STATUS "Doxygen: graphviz dot found at ${DOXYGEN_DOT_PATH}")
  else ()
    set(DOXYGEN_HAVE_DOT NO)
    message(STATUS "Doxygen: graphviz dot not found — diagrams disabled")
  endif ()

  doxygen_add_docs(doxygen
        ${PROJECT_SOURCE_DIR}/README.md
        ${PROJECT_SOURCE_DIR}/README_CN.md
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
        COMMENT "Generate Doxygen API documentation"
    )

  message(STATUS "Doxygen: enabled — use 'make doxygen' to build")
else ()
  message(STATUS "Doxygen: disabled (use -DYADDNSC_BUILD_DOCS=ON to enable)")
endif ()
