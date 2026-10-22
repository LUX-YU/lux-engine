cmake_minimum_required(VERSION 3.25)

function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    message("${ARGV}\n${output}${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Execution consumer failed: ${result}")
    endif()
endfunction()

file(REAL_PATH "${BUILD_DIR}" build_root)
set(root "${build_root}/execution-standalone")
set(prefix "${root}/original")
set(relocated "${root}/relocated")
# Reuse compiled objects, but qualify a fresh install rather than stale public headers.
# Both disposable prefixes are fixed children of this test's build directory.
file(REMOVE_RECURSE "${prefix}" "${relocated}")
set(common
    -G "${GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
    "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
    "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}"
    "-Dlux-cxx_DIR=${DEPENDENCY_DIR}"
    "-Dstdexec_DIR=${STDEXEC_DIR}"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
    -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON
)

# Build only these production dependencies; their full behavior tests run in the parent build.
run_checked("${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/source" -B "${root}/source" ${common}
    "-DLUX_SOURCE_DIR=${SOURCE_DIR}" "-Dlux-cmake-toolset_DIR=${TOOLSET_DIR}"
    -DBUILD_TESTING=OFF "-DCMAKE_INSTALL_PREFIX=${prefix}"
)
run_checked("${CMAKE_COMMAND}" --build "${root}/source" --target all -j 4 -- -k 0)
run_checked("${CMAKE_COMMAND}" --build "${root}/source" --target all -j 4 -- -k 0)
run_checked("${CMAKE_COMMAND}" --install "${root}/source")
file(RENAME "${prefix}" "${relocated}")
if(EXISTS "${relocated}/include/lux/engine/process/ExecutionRuntime.hpp")
    message(FATAL_ERROR "Obsolete Execution header installed")
endif()
run_checked("${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/installed" -B "${root}/consumer" ${common}
    "-DCMAKE_PREFIX_PATH=${relocated}"
)
run_checked("${CMAKE_COMMAND}" --build "${root}/consumer" --target all -j 4 -- -k 0)
run_checked("${CMAKE_COMMAND}" --build "${root}/consumer" --target all -j 4 -- -k 0)
run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${root}/consumer" --output-on-failure -j 1)
