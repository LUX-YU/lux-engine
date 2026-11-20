include_guard(GLOBAL)

# Shader generation uses the shared pinned Slang compiler. Runtime SPIR-V qualification
# uses fixed SPIRV-Tools sources and the explicitly resolved SPIRV-Cross package.
if(NOT TARGET shader_compiler)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../../resource/shader_metadata" "${CMAKE_BINARY_DIR}/shader-compiler")
endif()

foreach(package core glsl hlsl msl cpp reflect c)
    find_package(spirv_cross_${package} CONFIG REQUIRED)
endforeach()

find_file(LUX_SPIRV_CROSS_LICENSE NAMES copyright LICENSE LICENSE.txt
    HINTS "${spirv_cross_c_DIR}/../spirv-cross" "${spirv_cross_c_DIR}"
)

if(LUX_SPIRV_CROSS_LICENSE)
    install(FILES "${LUX_SPIRV_CROSS_LICENSE}"
        DESTINATION share/lux-engine-render/licenses
        RENAME SPIRV-Cross.txt
        COMPONENT lux_engine_render
    )
endif()

include(FetchContent)
get_filename_component(shader_repo "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
file(READ "${shader_repo}/bootstrap/shader-tools.json" shader_tools_lock)
get_filename_component(default_dependency_cache "${shader_repo}/../build/dependencies" ABSOLUTE)
set(LUX_RENDER_DEPENDENCY_CACHE "${default_dependency_cache}" CACHE PATH "Shared Render dependency cache")
foreach(name spirv_headers spirv_tools)
    string(JSON source_url GET "${shader_tools_lock}" ${name} url)
    string(JSON source_hash GET "${shader_tools_lock}" ${name} sha256)
    FetchContent_Declare(${name}
        URL "${source_url}" URL_HASH "SHA256=${source_hash}"
        SOURCE_DIR "${LUX_RENDER_DEPENDENCY_CACHE}/${name}-src"
        BINARY_DIR "${CMAKE_BINARY_DIR}/${name}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        EXCLUDE_FROM_ALL
    )
endforeach()
FetchContent_MakeAvailable(spirv_headers)
set(SPIRV-Headers_SOURCE_DIR "${spirv_headers_SOURCE_DIR}")
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spirv_tools)

install(FILES "${spirv_tools_SOURCE_DIR}/LICENSE"
    DESTINATION share/lux-engine-render/licenses
    RENAME SPIRV-Tools.txt
    COMPONENT lux_engine_render
)

install(FILES "${spirv_headers_SOURCE_DIR}/LICENSE"
    DESTINATION share/lux-engine-render/licenses
    RENAME SPIRV-Headers.txt
    COMPONENT lux_engine_render
)

function(lux_render_shader output source stage symbol)
    set(shader_options ${ARGN})
    get_filename_component(render_shader_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    get_filename_component(shader_source "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(shader_depfile "${output}.d")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND lux_shader_compile "${shader_source}" main ${stage} bundle "${output}"
            "--symbol=${symbol}" "--depfile=${shader_depfile}" "-I${render_shader_root}/../material/shaders"
            ${shader_options}
        DEPENDS "${shader_source}" lux_shader_compile "$<TARGET_FILE:shader_compiler>"
        DEPFILE "${shader_depfile}"
        VERBATIM
    )
endfunction()
