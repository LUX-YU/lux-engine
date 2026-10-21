find_package(lux-cxx REQUIRED COMPONENTS compile_time core)
option(LUX_MATERIAL_BUILD_COOKER "Build the Material asset cooker in addition to the compiler" ON)
find_package(Vulkan QUIET COMPONENTS shaderc_combined)
find_package(unofficial-shaderc CONFIG QUIET)
find_package(shaderc CONFIG QUIET)

# Use shaderc's C ABI DLL on Windows. A prebuilt combined archive mixes its STL
# instantiations with the caller's CRT/ASan annotations in the same binary.
if(MSVC)
    find_path(LUX_MATERIAL_SHADERC_INCLUDE shaderc/shaderc.hpp
        HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED
    )
    get_filename_component(shaderc_sdk "${LUX_MATERIAL_SHADERC_INCLUDE}/.." ABSOLUTE)
    find_library(LUX_MATERIAL_SHADERC_SHARED shaderc_shared HINTS "${shaderc_sdk}/Lib" NO_DEFAULT_PATH REQUIRED)
    find_library(
        LUX_MATERIAL_SHADERC_SHARED_DEBUG NAMES shaderc_sharedd
        HINTS "${shaderc_sdk}/Lib" NO_DEFAULT_PATH REQUIRED
    )
    find_file(LUX_MATERIAL_SHADERC_DLL shaderc_shared.dll HINTS "${shaderc_sdk}/Bin" NO_DEFAULT_PATH REQUIRED)
    find_file(
        LUX_MATERIAL_SHADERC_DEBUG_DLL NAMES shaderc_sharedd.dll
        HINTS "${shaderc_sdk}/Bin" NO_DEFAULT_PATH REQUIRED
    )
    add_library(lux_material_shaderc SHARED IMPORTED)
    set_target_properties(lux_material_shaderc PROPERTIES
        IMPORTED_CONFIGURATIONS "Debug;Release"
        IMPORTED_IMPLIB_RELEASE "${LUX_MATERIAL_SHADERC_SHARED}"
        IMPORTED_IMPLIB_DEBUG "${LUX_MATERIAL_SHADERC_SHARED_DEBUG}"
        IMPORTED_LOCATION_RELEASE "${LUX_MATERIAL_SHADERC_DLL}"
        IMPORTED_LOCATION_DEBUG "${LUX_MATERIAL_SHADERC_DEBUG_DLL}"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release
        INTERFACE_INCLUDE_DIRECTORIES "${LUX_MATERIAL_SHADERC_INCLUDE}"
        INTERFACE_COMPILE_DEFINITIONS SHADERC_SHAREDLIB
    )
    set(lux_material_shaderc_target lux_material_shaderc)
elseif(TARGET unofficial::shaderc::shaderc)
    set(lux_material_shaderc_target unofficial::shaderc::shaderc)
elseif(TARGET Vulkan::shaderc_combined)
    set(lux_material_shaderc_target Vulkan::shaderc_combined)
elseif(TARGET shaderc::shaderc)
    set(lux_material_shaderc_target shaderc::shaderc)
else()
    message(FATAL_ERROR "toolchain_material_compiler requires a shaderc target")
endif()

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_MATERIAL_COMPILER_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_MATERIAL_COMPILER_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_MATERIAL_COMPILER_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/material/compiler/visibility.h
)
generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_MATERIAL_COOKER_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_MATERIAL_COOKER_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_MATERIAL_COOKER_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/material/cooker/visibility.h
)

set(lux_material_shader_source_dir
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/function/material/shaders"
)
set(lux_material_shader_emitted_dir "${CMAKE_BINARY_DIR}/material_lglsl_emitted")
set(lux_material_lighting_source "${lux_material_shader_source_dir}/lighting_common.lglslh")
set(lux_material_lighting_emitted "${lux_material_shader_emitted_dir}/lighting_common.glsl")
set(lux_material_view_prefix_source "${lux_material_shader_source_dir}/view_pc_prefix.lglslh")
set(lux_material_view_prefix_emitted "${lux_material_shader_emitted_dir}/view_pc_prefix.glsl")
set(lux_material_shadow_common_source "${lux_material_shader_source_dir}/shadow_common.lglslh")
set(lux_material_shadow_common_emitted "${lux_material_shader_emitted_dir}/shadow_common.glsl")
set(lux_material_cluster_common_source "${lux_material_shader_source_dir}/cluster_common.lglslh")
set(lux_material_cluster_common_emitted "${lux_material_shader_emitted_dir}/cluster_common.glsl")
file(MAKE_DIRECTORY "${lux_material_shader_emitted_dir}")

foreach(shader_include IN ITEMS lighting view_prefix shadow_common cluster_common)
    set(source "${lux_material_${shader_include}_source}")
    set(output "${lux_material_${shader_include}_emitted}")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND $<TARGET_FILE:lux_shader_emitter> --in "${source}" --out "${output}" --header
        DEPENDS lux_shader_emitter "${source}"
        COMMENT "Emit Material compiler ${shader_include}.glsl"
        VERBATIM
    )
endforeach()
add_custom_target(
    toolchain_material_shader_includes
    DEPENDS
        "${lux_material_lighting_emitted}"
        "${lux_material_view_prefix_emitted}"
        "${lux_material_shadow_common_emitted}"
        "${lux_material_cluster_common_emitted}"
)

set(lux_material_embedded_includes
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lux/engine/material/compiler/MaterialShaderIncludes.generated.hpp"
)
set(lux_material_raw_includes
    "${lux_material_shader_source_dir}/brdf/brdf_common.glsl"
    "${lux_material_shader_source_dir}/brdf/brdf_ggx.glsl"
    "${lux_material_shader_source_dir}/brdf/brdf_toon.glsl"
    "${lux_material_shader_source_dir}/gbuffer_encode.glsl"
    "${lux_material_shader_source_dir}/light_types.glsl"
    "${lux_material_shader_source_dir}/lighting_tbn.glsl"
    "${lux_material_shader_source_dir}/material_types.glsl"
    "${lux_material_shader_source_dir}/shadow_evsm.glsl"
    "${lux_material_shader_source_dir}/shadow_pcf.glsl"
    "${lux_material_shader_source_dir}/texture_sampling.glsl"
    "${lux_material_shader_source_dir}/transition_dither.glsl"
    "${lux_material_shader_source_dir}/view_common.glsl"
)
add_custom_command(
    OUTPUT "${lux_material_embedded_includes}"
    COMMAND
        ${CMAKE_COMMAND}
        "-DOUTPUT=${lux_material_embedded_includes}"
        "-DSOURCE_ROOT=${lux_material_shader_source_dir}"
        "-DGENERATED_ROOT=${lux_material_shader_emitted_dir}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../shader/cmake/GenerateEmbeddedShaderIncludes.cmake"
    DEPENDS
        ${lux_material_raw_includes}
        "${lux_material_lighting_emitted}"
        "${lux_material_view_prefix_emitted}"
        "${lux_material_shadow_common_emitted}"
        "${lux_material_cluster_common_emitted}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../shader/cmake/GenerateEmbeddedShaderIncludes.cmake"
    COMMENT "Embed Material compiler Shader includes"
    VERBATIM
)

add_component(
    COMPONENT_NAME toolchain_material_compiler
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_material_compiler
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderEmitter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderCompiler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialCompiler.cpp
        "${lux_material_embedded_includes}"
)
add_dependencies(toolchain_material_compiler toolchain_material_shader_includes)
component_include_directories(
    toolchain_material_compiler
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../shader/sinclude
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_compile_definitions(toolchain_material_compiler PRIVATE LUX_ENGINE_MATERIAL_COMPILER_LIBRARY)
if(TARGET lux_material_shaderc)
    add_custom_command(TARGET toolchain_material_compiler POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:lux_material_shaderc>" "$<TARGET_FILE_DIR:toolchain_material_compiler>"
        VERBATIM
    )
endif()

target_link_libraries(
    toolchain_material_compiler
    PUBLIC
        lux::engine::function::material_graph
        lux::engine::resource::description_types
        lux::engine::resource::shader_metadata
        lux::engine::resource::identity
    PRIVATE
        toolchain_shader_reflection
        ${lux_material_shaderc_target}
)
component_add_transitive_commands(
    toolchain_material_compiler
    "find_package(lux-engine-function REQUIRED COMPONENTS material_graph)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description_types shader_metadata identity)"
)

if(LUX_MATERIAL_BUILD_COOKER)
    find_package(Eigen3 CONFIG REQUIRED)
    add_component(
        STATIC
        COMPONENT_NAME toolchain_material_cooker
        NAMESPACE      lux::engine::toolchain
        OUTPUT_NAME    lux_engine_toolchain_material_cooker
        SOURCE_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialToGraph.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialCooker.cpp
    )
    component_include_directories(
        toolchain_material_cooker
        BUILD_TIME_EXPORT
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${LUX_GENERATE_HEADER_DIR}
        INSTALL_TIME include
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    )
    target_compile_definitions(
        toolchain_material_cooker
        PUBLIC LUX_ENGINE_MATERIAL_COOKER_DLL_DISABLE
        PRIVATE LUX_ENGINE_MATERIAL_COOKER_LIBRARY
    )
    target_link_libraries(
        toolchain_material_cooker
        PUBLIC
            lux::engine::toolchain::toolchain_material_compiler
            lux::engine::resource::asset
            Eigen3::Eigen
    )
    component_add_transitive_commands(
        toolchain_material_cooker
        "find_package(lux-engine-toolchain-material REQUIRED COMPONENTS toolchain_material_compiler)"
        "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
        "find_package(Eigen3 CONFIG REQUIRED)"
    )
endif()

if(LUX_PROFILE_HAS_TOOLCHAIN)
    if(TARGET lux_material_shaderc)
        install(FILES "$<TARGET_FILE:lux_material_shaderc>" DESTINATION bin COMPONENT lux_toolchain)
    endif()
    set(material_components toolchain_material_compiler)
    if(LUX_MATERIAL_BUILD_COOKER)
        list(APPEND material_components toolchain_material_cooker)
    endif()
    lux_engine_install_components(
        PROJECT_NAME lux-engine-toolchain-material
        VERSION      ${PROJECT_VERSION}
        NAMESPACE    lux::engine::toolchain
        COMPONENTS ${material_components}
    )
endif()

if(BUILD_TESTING AND LUX_EDITOR_BUILD_TOOLCHAIN_TESTS)
    add_executable(material_domain_compiler_test test/domain_boundary.cpp)

    target_link_libraries(material_domain_compiler_test PRIVATE lux::engine::toolchain::toolchain_material_compiler)

    add_test(
        NAME material.domain_compiler
        COMMAND material_domain_compiler_test "${CMAKE_CURRENT_BINARY_DIR}/material.spirv.bin"
    )
endif()
