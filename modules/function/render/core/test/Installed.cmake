cmake_minimum_required(VERSION 3.22)

function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    message("${ARGV}\n${output}${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Installed consumer command failed: ${result}")
    endif()
endfunction()

# Fresh directories also prevent previous installs from supplying missing headers.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(root "${BUILD_DIR}/installed-check-${nonce}")
set(prefix "${root}/original")
set(relocated "${root}/relocated")

run_checked("${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}" --prefix "${prefix}")

file(RENAME "${prefix}" "${relocated}")

# Resolve the same actual header-only dependency as the parent build. Package registries remain disabled.
file(STRINGS "${BUILD_DIR}/CMakeCache.txt" stdexec_cache REGEX "^stdexec_DIR:[^=]+=")
string(REGEX REPLACE "^stdexec_DIR:[^=]+=" "" stdexec_directory "${stdexec_cache}")

run_checked(
    "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${root}/build" -G "${GENERATOR}"
    "-Dlux-engine-render_DIR=${relocated}/share/lux-engine-render"
    "-DCMAKE_PREFIX_PATH=${relocated}" "-Dstdexec_DIR=${stdexec_directory}"
    "-Dlux-cxx_DIR=${DEPENDENCY_DIR}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
    "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
)

run_checked("${CMAKE_COMMAND}" --build "${root}/build" --config "${CONFIG}" --target all -j 4 -- -k 0)

run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${root}/build" -C "${CONFIG}" --output-on-failure)
