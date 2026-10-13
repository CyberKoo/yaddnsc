cmake_minimum_required(VERSION 3.28)
if(NOT DEFINED WORK_DIR OR NOT DEFINED SDK_PREFIX)
    message(FATAL_ERROR "Pass WORK_DIR and SDK_PREFIX (an installed SDK)")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")

function(run)
    set(arguments)
    math(EXPR last "${ARGC} - 1")
    foreach(index RANGE 0 ${last})
        string(REPLACE ";" "\\;" argument "${ARGV${index}}")
        list(APPEND arguments "${argument}")
    endforeach()
    if("${ARGV1}" STREQUAL "-S")
        list(APPEND arguments --fresh)
    endif()
    execute_process(COMMAND ${arguments} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${ARGV}\n${output}\n${error}")
    endif()
endfunction()

# Use installed examples after copying them outside the checkout/install tree.
file(COPY "${SDK_PREFIX}/share/yaddnsc/examples/" DESTINATION "${WORK_DIR}/examples")
set(example_source "${WORK_DIR}/examples")
run("${CMAKE_COMMAND}" -S "${example_source}" -B "${WORK_DIR}/consumer"
    "-DCMAKE_PREFIX_PATH=${SDK_PREFIX}" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF)
run("${CMAKE_COMMAND}" --build "${WORK_DIR}/consumer" --parallel 2)
run("${CMAKE_CTEST_COMMAND}" --test-dir "${WORK_DIR}/consumer" --output-on-failure)
run("${CMAKE_COMMAND}" -S "${example_source}" -B "${WORK_DIR}/c-only"
    "-DCMAKE_PREFIX_PATH=${SDK_PREFIX}" -DYADDNSC_CONSUMER_C_ONLY=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_glaze=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE)
run("${CMAKE_COMMAND}" --build "${WORK_DIR}/c-only" --parallel 2)

if(VERIFY_OLD_CXX_ABI)
    run("${CMAKE_COMMAND}" -S "${example_source}" -B "${WORK_DIR}/other-abi"
        "-DCMAKE_PREFIX_PATH=${SDK_PREFIX}" -DYADDNSC_CONSUMER_OLD_CXX_ABI=ON)
    run("${CMAKE_COMMAND}" --build "${WORK_DIR}/other-abi" --parallel 2)
    run("${CMAKE_CTEST_COMMAND}" --test-dir "${WORK_DIR}/other-abi" --output-on-failure)
endif()

# Generate a configure-only probe for optional/required dependency semantics.
file(MAKE_DIRECTORY "${WORK_DIR}/probe")
file(WRITE "${WORK_DIR}/probe/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(component_probe LANGUAGES NONE)
if(PROBE_OPTIONAL)
    find_package(yaddnsc REQUIRED CONFIG COMPONENTS plugin_abi plugin_sdk
        OPTIONAL_COMPONENTS plugin_sdk_xml plugin_sdk_crypto)
    if(yaddnsc_plugin_sdk_xml_FOUND OR yaddnsc_plugin_sdk_crypto_FOUND)
        message(FATAL_ERROR "Missing optional dependencies reported as available")
    endif()
elseif(PROBE_REQUIRED_COMPONENT)
    find_package(yaddnsc REQUIRED CONFIG COMPONENTS ${PROBE_REQUIRED_COMPONENT})
else()
    find_package(yaddnsc REQUIRED CONFIG COMPONENTS plugin_sdk)
endif()
file(WRITE "${CMAKE_BINARY_DIR}/required-glaze-version.txt" "${yaddnsc_GLAZE_VERSION}")
]=])
run("${CMAKE_COMMAND}" -S "${WORK_DIR}/probe" -B "${WORK_DIR}/optional"
    "-DCMAKE_PREFIX_PATH=${SDK_PREFIX}" -DPROBE_OPTIONAL=ON
    -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_LibXml2=TRUE)

foreach(component plugin_sdk_xml plugin_sdk_crypto)
    if(component STREQUAL "plugin_sdk_xml")
        set(dependency LibXml2)
    else()
        set(dependency OpenSSL)
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --fresh -S "${WORK_DIR}/probe"
        -B "${WORK_DIR}/required-${component}" "-DCMAKE_PREFIX_PATH=${SDK_PREFIX}"
        "-DPROBE_REQUIRED_COMPONENT=${component}" "-DCMAKE_DISABLE_FIND_PACKAGE_${dependency}=TRUE"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(status EQUAL 0)
        message(FATAL_ERROR "Missing required component ${component} was accepted")
    endif()
endforeach()

# Signing can be consumed without Glaze or the combined C++ Driver helpers.
file(MAKE_DIRECTORY "${WORK_DIR}/crypto-only")
file(WRITE "${WORK_DIR}/crypto-only/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(crypto_only LANGUAGES CXX)
find_package(yaddnsc REQUIRED CONFIG COMPONENTS plugin_sdk_crypto)
add_executable(hash hash.cpp)
target_link_libraries(hash PRIVATE yaddnsc::plugin_sdk_crypto)
]=])
file(WRITE "${WORK_DIR}/crypto-only/hash.cpp"
    "#include <yaddnsc/sdk/signing.hpp>\nint main() { return yaddnsc::sdk::signing::sha256_hex(\"consumer\").empty(); }\n")
run("${CMAKE_COMMAND}" -S "${WORK_DIR}/crypto-only" -B "${WORK_DIR}/crypto-only-build"
    "-DCMAKE_PREFIX_PATH=${SDK_PREFIX}" -DCMAKE_DISABLE_FIND_PACKAGE_glaze=TRUE)
run("${CMAKE_COMMAND}" --build "${WORK_DIR}/crypto-only-build" --parallel 2)
run("${WORK_DIR}/crypto-only-build/hash")

file(READ "${WORK_DIR}/optional/required-glaze-version.txt" required_glaze_version)
if(NOT required_glaze_version)
    message(FATAL_ERROR "Installed SDK did not declare its required Glaze version")
endif()

# A mismatched candidate must be skipped in favour of the bundled exact release.
set(conflict "${WORK_DIR}/conflict/lib/cmake/glaze")
file(MAKE_DIRECTORY "${conflict}")
file(WRITE "${conflict}/glazeConfig.cmake" "message(FATAL_ERROR \"Loaded incompatible Glaze\")\n")
file(WRITE "${conflict}/glazeConfigVersion.cmake" "set(PACKAGE_VERSION ${required_glaze_version}.999)\nset(PACKAGE_VERSION_COMPATIBLE FALSE)\nset(PACKAGE_VERSION_EXACT FALSE)\n")
run("${CMAKE_COMMAND}" -S "${WORK_DIR}/probe" -B "${WORK_DIR}/fallback"
    "-DCMAKE_PREFIX_PATH=${WORK_DIR}/conflict;${SDK_PREFIX}")

# Relocation and an install without Glaze metadata must give a useful diagnosis.
file(MAKE_DIRECTORY "${WORK_DIR}/relocated/include" "${WORK_DIR}/relocated/share")
file(COPY "${SDK_PREFIX}/include/yaddnsc" "${SDK_PREFIX}/include/glaze"
    DESTINATION "${WORK_DIR}/relocated/include")
file(COPY "${SDK_PREFIX}/share/glaze" DESTINATION "${WORK_DIR}/relocated/share")
file(GLOB_RECURSE sdk_configs "${SDK_PREFIX}/lib*/yaddnscConfig.cmake")
foreach(config IN LISTS sdk_configs)
    get_filename_component(directory "${config}" DIRECTORY)
    file(RELATIVE_PATH relative "${SDK_PREFIX}" "${directory}")
    file(COPY "${directory}/" DESTINATION "${WORK_DIR}/relocated/${relative}")
    set(relocated_package_dir "${WORK_DIR}/relocated/${relative}")
endforeach()
run("${CMAKE_COMMAND}" -S "${example_source}" -B "${WORK_DIR}/relocation"
    "-DCMAKE_PREFIX_PATH=${WORK_DIR}/relocated" "-Dyaddnsc_DIR=${relocated_package_dir}"
    "-Dglaze_DIR=${WORK_DIR}/relocated/share/glaze")
run("${CMAKE_COMMAND}" --build "${WORK_DIR}/relocation" --parallel 2)
run("${CMAKE_CTEST_COMMAND}" --test-dir "${WORK_DIR}/relocation" --output-on-failure)
file(GLOB_RECURSE glaze_configs "${WORK_DIR}/relocated/*glazeConfig.cmake")
if(NOT glaze_configs)
    message(FATAL_ERROR "Development package did not bundle Glaze metadata")
endif()
foreach(config IN LISTS glaze_configs)
    get_filename_component(directory "${config}" DIRECTORY)
    file(REMOVE_RECURSE "${directory}")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" --fresh -S "${WORK_DIR}/probe" -B "${WORK_DIR}/mismatch"
    "-DCMAKE_PREFIX_PATH=${WORK_DIR}/conflict;${WORK_DIR}/relocated"
    "-DCMAKE_IGNORE_PREFIX_PATH=${SDK_PREFIX}" -DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=OFF -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF
    -DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=OFF -DCMAKE_MAKE_PROGRAM=/usr/bin/make
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
string(FIND "${output}${error}" "requires exactly Glaze ${required_glaze_version}" diagnostic_position)
if(status EQUAL 0 OR diagnostic_position EQUAL -1)
    message(FATAL_ERROR "Expected exact-version SDK diagnostic, got ${status}: ${output}${error}")
endif()

file(GLOB_RECURSE archives "${SDK_PREFIX}/*signing*.a" "${SDK_PREFIX}/*plugin_sdk_crypto*.a" "${SDK_PREFIX}/*plugin_support_crypto*.a")
if(archives)
    message(FATAL_ERROR "Precompiled crypto archive is still distributed: ${archives}")
endif()
message(STATUS "Installed SDK consumers, optional dependencies, exact version and relocation passed")
