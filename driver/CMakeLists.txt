cmake_minimum_required(VERSION 3.22...3.31 FATAL_ERROR)

# ==============================================================================
# Driver modules
# ==============================================================================
add_custom_target(drivers)

# discover all driver subdirectories
file(GLOB DRIVER_DIRS "./*" LIST_DIRECTORIES true)
foreach (DIR_NAME ${DRIVER_DIRS})
  if (IS_DIRECTORY ${DIR_NAME})
    get_filename_component(DRIVER_NAME ${DIR_NAME} NAME)
    message(STATUS "Build driver ${DRIVER_NAME}")

    add_subdirectory(${DIR_NAME})

    # Output filename is <name>.so (no lib prefix) so the driver loader can
    # find it by just <name>.  The CMake target is also named <name>.
    set_target_properties(${DRIVER_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${DRIVER_NAME}")

    # -----------------------------------------------------------------------
    # Compiler options
    # -----------------------------------------------------------------------
    target_compile_options(${DRIVER_NAME} PRIVATE
                -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Werror
                # Hide all symbols by default; only YADDNSC_EXPORT-marked functions
                # have STV_DEFAULT visibility.
                -fvisibility=hidden
                # Debug symbols
                $<$<CONFIG:Debug>:-ggdb -gdwarf-3>
                # Clang 19.x needs -fexperimental-library for std::stop_token, std::jthread, etc.
                $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,19>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,20>>:-fexperimental-library>
        )

    # -----------------------------------------------------------------------
    # Linker options
    # -----------------------------------------------------------------------
    # Allow undefined symbols at runtime (resolved from the main executable at dlopen time)
    target_link_options(${DRIVER_NAME} PRIVATE
                $<$<PLATFORM_ID:Darwin>:-Wl,-undefined,dynamic_lookup>
        )
    # Strip symbols in Release builds
    target_link_options(${DRIVER_NAME} PRIVATE
                $<$<CONFIG:Release>:$<$<PLATFORM_ID:Darwin>:-Wl,-S>>
                $<$<CONFIG:Release>:$<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,-s>>
        )

    # -----------------------------------------------------------------------
    # Target properties
    # -----------------------------------------------------------------------
    # LTO is incompatible with musl (auto-detected in root FeatureCheck)
    if (NOT LIBC_MUSL)
      set_target_properties(${DRIVER_NAME} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif ()

    # -----------------------------------------------------------------------
    # Include directories
    # -----------------------------------------------------------------------
    target_include_directories(${DRIVER_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated/)
    if (glaze_ADDED)
      target_include_directories(${DRIVER_NAME} SYSTEM PRIVATE ${glaze_SOURCE_DIR}/include)
    endif ()

    # -----------------------------------------------------------------------
    # Link libraries
    # -----------------------------------------------------------------------
    target_link_libraries(${DRIVER_NAME} PRIVATE yaddnsc_fmt)

    # -----------------------------------------------------------------------
    # Aggregate all drivers under the top-level 'drivers' target
    # -----------------------------------------------------------------------
    add_dependencies(drivers ${DRIVER_NAME})

    # -----------------------------------------------------------------------
    # Install rules
    # -----------------------------------------------------------------------
    install(TARGETS ${DRIVER_NAME} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/yaddnsc/drivers)
  endif ()
endforeach ()
