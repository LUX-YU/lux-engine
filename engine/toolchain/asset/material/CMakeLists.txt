find_package(lux-cxx REQUIRED COMPONENTS compile_time core)
find_package(Vulkan QUIET COMPONENTS shaderc_combined)
find_package(unofficial-shaderc CONFIG QUIET)
find_package(shaderc CONFIG QUIET)

if(TARGET unofficial::shaderc::shaderc)
    set(lux_material_shaderc_target unofficial::shaderc::shaderc)
elseif(TARGET Vulkan::shaderc_combined)
    set(lux_material_shaderc_target Vulkan::shaderc_combined)
elseif(TARGET shaderc::shaderc)
    set(lux_material_shaderc_target shaderc::shaderc)
else()
    message(FATAL_ERROR "toolchain_material_cooker requires a shaderc target")
endif()

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_MATERIAL_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_MATERIAL_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TOOLCHAIN_MATERIAL_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/asset/material/visibility.h
)

set(lux_material_shader_source_dir
    "${CMAKE_SOURCE_DIR}/modules/function/render/features/assets/shaders"
)
set(lux_material_shader_emitted_dir "${CMAKE_BINARY_DIR}/lglsl_emitted")
set(lux_material_lighting_source
    "${lux_material_shader_source_dir}/lighting_common.lglslh"
)
set(lux_material_lighting_emitted
    "${lux_material_shader_emitted_dir}/lighting_common.glsl"
)
set(lux_material_view_prefix_source
    "${lux_material_shader_source_dir}/view_pc_prefix.lglslh"
)
set(lux_material_view_prefix_emitted
    "${lux_material_shader_emitted_dir}/view_pc_prefix.glsl"
)
set(lux_material_shadow_common_source
    "${lux_material_shader_source_dir}/shadow_common.lglslh"
)
set(lux_material_shadow_common_emitted
    "${lux_material_shader_emitted_dir}/shadow_common.glsl"
)
set(lux_material_cluster_common_source
    "${lux_material_shader_source_dir}/cluster_common.lglslh"
)
set(lux_material_cluster_common_emitted
    "${lux_material_shader_emitted_dir}/cluster_common.glsl"
)
file(MAKE_DIRECTORY "${lux_material_shader_emitted_dir}")

add_custom_command(
    OUTPUT "${lux_material_lighting_emitted}"
    COMMAND $<TARGET_FILE:lux_shader_emitter>
        --in "${lux_material_lighting_source}"
        --out "${lux_material_lighting_emitted}"
        --header
    DEPENDS
        lux_shader_emitter
        "${lux_material_lighting_source}"
    COMMENT "Emit material compiler lighting_common.glsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${lux_material_view_prefix_emitted}"
    COMMAND $<TARGET_FILE:lux_shader_emitter>
        --in "${lux_material_view_prefix_source}"
        --out "${lux_material_view_prefix_emitted}"
        --header
    DEPENDS
        lux_shader_emitter
        "${lux_material_view_prefix_source}"
    COMMENT "Emit material compiler view_pc_prefix.glsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${lux_material_shadow_common_emitted}"
    COMMAND $<TARGET_FILE:lux_shader_emitter>
        --in "${lux_material_shadow_common_source}"
        --out "${lux_material_shadow_common_emitted}"
        --header
    DEPENDS lux_shader_emitter "${lux_material_shadow_common_source}"
    COMMENT "Emit material compiler shadow_common.glsl"
    VERBATIM
)
add_custom_command(
    OUTPUT "${lux_material_cluster_common_emitted}"
    COMMAND $<TARGET_FILE:lux_shader_emitter>
        --in "${lux_material_cluster_common_source}"
        --out "${lux_material_cluster_common_emitted}"
        --header
    DEPENDS lux_shader_emitter "${lux_material_cluster_common_source}"
    COMMENT "Emit material compiler cluster_common.glsl"
    VERBATIM
)
add_custom_target(
    toolchain_material_shader_includes
    DEPENDS
        "${lux_material_lighting_emitted}"
        "${lux_material_view_prefix_emitted}"
        "${lux_material_shadow_common_emitted}"
        "${lux_material_cluster_common_emitted}"
)
lux_classify_target(
    TARGET  toolchain_material_shader_includes
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    FOUNDATION
)

add_component(
    STATIC
    COMPONENT_NAME toolchain_material_cooker
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_material_cooker
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderIR.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderEmitter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderCompiler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialLowering.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialToGraph.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialCooker.cpp
)
add_dependencies(toolchain_material_cooker toolchain_material_shader_includes)

component_include_directories(
    toolchain_material_cooker
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../shader/pinclude
)
target_compile_definitions(
    toolchain_material_cooker
    PUBLIC LUX_ENGINE_TOOLCHAIN_MATERIAL_DLL_DISABLE
    PRIVATE
        LUX_ENGINE_TOOLCHAIN_MATERIAL_LIBRARY
        LUX_MATERIAL_SHADER_SOURCE_DIR="${lux_material_shader_source_dir}"
        LUX_MATERIAL_SHADER_EMITTED_DIR="${lux_material_shader_emitted_dir}"
)
target_link_libraries(
    toolchain_material_cooker
    PUBLIC
        lux::engine::authoring::authoring_material
        lux::engine::resource::asset
    PRIVATE
        lux::engine::toolchain::toolchain_shader_asset_cooker
        ${lux_material_shaderc_target}
)
component_add_transitive_commands(
    toolchain_material_cooker
    "find_package(lux-engine-authoring REQUIRED COMPONENTS authoring_material)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
)
lux_classify_target(
    TARGET  toolchain_material_cooker
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

add_executable(
    toolchain_material_cooker_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/MaterialCookerTest.cpp
)
target_link_libraries(
    toolchain_material_cooker_test
    PRIVATE lux::engine::toolchain::toolchain_material_cooker
)
if(MSVC)
    target_compile_options(toolchain_material_cooker_test PRIVATE /UNDEBUG)
else()
    target_compile_options(toolchain_material_cooker_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  toolchain_material_cooker_test
    LAYER   TOOLCHAIN
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(NAME toolchain_material_cooker_test COMMAND toolchain_material_cooker_test)
set_tests_properties(toolchain_material_cooker_test PROPERTIES TIMEOUT 60)
