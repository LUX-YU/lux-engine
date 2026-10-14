cmake_minimum_required(VERSION 3.22)

function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    message("${ARGV}\n${output}${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "IPC installed consumer failed: ${result}")
    endif()
endfunction()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(root "${BUILD_DIR}/ipc-installed-${nonce}")
run_checked("${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${root}/original" --config "${CONFIG}")
file(RENAME "${root}/original" "${root}/relocated")

run_checked(
    "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${root}/build" -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${root}/relocated"
    "-Dlux-cxx_DIR=${DEPENDENCY_DIR}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
    "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
)

run_checked("${CMAKE_COMMAND}" --build "${root}/build" --target all -j 4 -- -k 0)
set(ENV{PATH} "${root}/relocated/bin;$ENV{PATH}")
run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${root}/build" --output-on-failure)
