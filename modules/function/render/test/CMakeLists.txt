# Formal SDK consumers only. Mechanism/fault/installation/performance probes live in the external verification suite.
add_executable(render_sync_client SyncClient.cpp)
target_link_libraries(render_sync_client PRIVATE render_commands lux::engine::platform::platform_ipc)
add_test(NAME render.client.sync_source COMMAND render_sync_client)
set_target_properties(render_sync_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

if(LUX_RENDER_CLIENT_ONLY)
    return()
endif()

add_executable(render_output_test Output.cpp)
target_link_libraries(render_output_test PRIVATE render_native_vulkan)
add_test(NAME render.native.output COMMAND render_output_test)
set_tests_properties(render.native.output PROPERTIES TIMEOUT 30)

get_filename_component(render_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_executable(render_material_asset_tool
    EmitMaterial.cpp
    "${render_source_root}/engine/toolchain/material/src/ShaderEmitter.cpp"
)
target_include_directories(render_material_asset_tool PRIVATE
    "${render_source_root}/engine/toolchain/material/include"
    "${render_source_root}/engine/toolchain/material/pinclude"
)
target_compile_definitions(render_material_asset_tool PRIVATE LUX_ENGINE_MATERIAL_COMPILER_DLL_DISABLE)
target_link_libraries(render_material_asset_tool PRIVATE lux::engine::function::material_graph)
generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_MATERIAL_COMPILER_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_MATERIAL_COMPILER_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_MATERIAL_COMPILER_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/material/compiler/visibility.h
)
set(shader_root "${render_source_root}/engine/toolchain/material/shaders")
set(shader "${CMAKE_CURRENT_BINARY_DIR}/assets/UnlitTexture.frag")
find_package(Vulkan REQUIRED COMPONENTS glslangValidator)
add_custom_command(
    OUTPUT "${shader}.spv"
    BYPRODUCTS "${shader}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets"
    COMMAND render_material_asset_tool "${shader}" texture
    COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S frag
        "-I${shader_root}" "${shader}" -o "${shader}.spv"
    DEPENDS render_material_asset_tool "${shader_root}/material_types.glsl"
        "${shader_root}/texture_sampling.glsl" "${shader_root}/transition_dither.glsl"
    VERBATIM
)
add_custom_target(render_sample_assets ALL DEPENDS "${shader}.spv")

add_library(render_basic_mesh_client OBJECT Client.cpp)
target_link_libraries(render_basic_mesh_client PRIVATE render_commands render_features lux::engine::platform::platform_ipc)
add_executable(render_basic_mesh Host.cpp $<TARGET_OBJECTS:render_basic_mesh_client>)
target_link_libraries(render_basic_mesh PRIVATE render_runtime render_native_vulkan_features lux::engine::core::core_execution)
add_dependencies(render_basic_mesh render_sample_assets)
add_test(NAME render.sample.basic_mesh COMMAND render_basic_mesh "${shader}.spv")
set_tests_properties(render.sample.basic_mesh PROPERTIES TIMEOUT 90)

# Real consumer DLLs, not SDK include overlays.
foreach(target render_sync_client render_output_test render_material_asset_tool render_basic_mesh)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
endforeach()
