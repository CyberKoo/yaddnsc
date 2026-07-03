cmake_minimum_required(VERSION 3.22...3.31 FATAL_ERROR)
project(drivers)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/objs/driver)

add_custom_target(drivers)

# On macOS the linker requires exported symbols from shared modules to be
# resolved at link time by default, but driver modules resolve them from
# the main executable at dlopen time.  Allow undefined symbols at link
# time so that symbols such as CoreLogger::* can be resolved at runtime.
if (APPLE)
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-undefined,dynamic_lookup")
endif ()

# discover all driver subdirectories
file(GLOB DRIVER_DIRS "./*" LIST_DIRECTORIES true)
foreach (DIR_NAME ${DRIVER_DIRS})
    if (IS_DIRECTORY ${DIR_NAME})
        get_filename_component(DRIVER_NAME ${DIR_NAME} NAME)
        message(STATUS "Build driver ${DRIVER_NAME}")

        add_subdirectory(${DIR_NAME})
        # Target is named drv_<name> to avoid collisions, but output filename is <name>.so
        # (no lib prefix, no drv_ prefix) so the driver loader can find it by just <name>
        set_target_properties(${DRIVER_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${DRIVER_NAME}")

        # Strict warnings for driver code, treated as errors
        target_compile_options(${DRIVER_NAME} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Werror
            # Inherited flags from the parent project (previously global, now target-local)
            $<$<CONFIG:Debug>:-ggdb -gdwarf-3>
            $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,19>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,20>>:-fexperimental-library>
        )
        target_link_options(${DRIVER_NAME} PRIVATE
            $<$<CONFIG:Release>:$<$<PLATFORM_ID:Darwin>:-Wl,-S>>
            $<$<CONFIG:Release>:$<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,-s>>
        )
        if (NOT LIBC_MUSL)
            set_target_properties(${DRIVER_NAME} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif ()

        # Include paths (previously global include_directories, now target-local)
        target_include_directories(${DRIVER_NAME} PRIVATE
            "${PROJECT_SOURCE_DIR}/../include"
            "${CMAKE_BINARY_DIR}/generated/"
        )
        if (glaze_ADDED)
            target_include_directories(${DRIVER_NAME} SYSTEM PRIVATE ${glaze_SOURCE_DIR}/include)
        endif ()

        # Aggregate all drivers under the top-level 'drivers' target
        add_dependencies(drivers ${DRIVER_NAME})
    endif ()
endforeach ()
