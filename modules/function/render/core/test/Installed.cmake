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

foreach(obsolete features/Texture.hpp features/Program.hpp data/Wire.hpp
    native/vulkan/transfer/GpuCompletion.hpp native/vulkan/CommandCompletion.hpp
)
    if(EXISTS "${relocated}/include/lux/engine/render/${obsolete}")
        message(FATAL_ERROR "Obsolete public header installed: ${obsolete}")
    endif()
endforeach()

# Resolve the same actual header-only dependency as the parent build. Package registries remain disabled.
file(STRINGS "${BUILD_DIR}/CMakeCache.txt" stdexec_cache REGEX "^stdexec_DIR:[^=]+=")
string(REGEX REPLACE "^stdexec_DIR:[^=]+=" "" stdexec_directory "${stdexec_cache}")

if(DEFINED CONSUMER_ASSET_DIR)
    if(NOT DEFINED CONSUMER_ASSETS)
        set(CONSUMER_ASSETS Unlit.frag.spv)
    endif()
    foreach(asset IN LISTS CONSUMER_ASSETS)
        file(COPY "${CONSUMER_ASSET_DIR}/${asset}" DESTINATION "${root}/assets")
        file(SHA256 "${CONSUMER_ASSET_DIR}/${asset}" source_hash)
        file(SHA256 "${root}/assets/${asset}" copied_hash)
        if(NOT source_hash STREQUAL copied_hash)
            message(FATAL_ERROR "Installed consumer asset changed: ${asset}")
        endif()
    endforeach()
    set(asset_argument "-DRENDER_TEST_ASSET_DIR=${root}/assets")
endif()

run_checked(
    "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${root}/build" -G "${GENERATOR}"
    "-Dlux-engine-render_DIR=${relocated}/share/lux-engine-render"
    "-DCMAKE_PREFIX_PATH=${relocated}" "-Dstdexec_DIR=${stdexec_directory}"
    "-Dlux-cxx_DIR=${DEPENDENCY_DIR}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
    "-DCMAKE_CXX_FLAGS=${CXX_FLAGS}" ${asset_argument}
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
)

run_checked("${CMAKE_COMMAND}" --build "${root}/build" --config "${CONFIG}" --target all -j 4 -- -k 0)

run_checked("${CMAKE_CTEST_COMMAND}" --test-dir "${root}/build" -C "${CONFIG}" -V --output-on-failure)
