# Formal SDK consumers only. Mechanism/fault/installation/performance probes live in the external verification suite.
add_executable(render_sync_client SyncClient.cpp)
target_link_libraries(render_sync_client PRIVATE render lux::engine::platform::platform_ipc)
add_test(NAME render.client.sync_source COMMAND render_sync_client)
set_target_properties(render_sync_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")


add_executable(render_output_test Output.cpp)
target_link_libraries(render_output_test PRIVATE render)
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
target_link_libraries(render_material_asset_tool PRIVATE
    lux::engine::function::material_graph
)
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
    OUTPUT "${shader}.spv" "${shader}.spv.meta"
    BYPRODUCTS "${shader}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets"
    COMMAND render_material_asset_tool "${shader}" texture
    COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S frag
        "-I${shader_root}" "${shader}" -o "${shader}.spv"
    COMMAND lux_shader_metadata "${shader}.spv" "${shader}.spv.meta"
    DEPENDS render_material_asset_tool lux_shader_metadata "${shader_root}/material_types.glsl"
        "${shader_root}/texture_sampling.glsl" "${shader_root}/transition_dither.glsl"
    VERBATIM
)
add_custom_target(render_sample_assets ALL DEPENDS "${shader}.spv" "${shader}.spv.meta")

add_library(render_basic_mesh_client OBJECT Client.cpp)
target_link_libraries(render_basic_mesh_client PRIVATE render lux::engine::platform::platform_ipc lux::engine::resource::shader_metadata)
add_executable(render_basic_mesh RenderSample.cpp $<TARGET_OBJECTS:render_basic_mesh_client>)
target_link_libraries(render_basic_mesh PRIVATE render lux::engine::core::core_execution lux::engine::resource::shader_metadata)
add_dependencies(render_basic_mesh render_sample_assets)
add_test(NAME render.sample.basic_mesh COMMAND render_basic_mesh "${shader}.spv")
set_tests_properties(render.sample.basic_mesh PROPERTIES TIMEOUT 90)

# Real consumer DLLs, not SDK include overlays.
foreach(target render_sync_client render_output_test render_material_asset_tool render_basic_mesh)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
endforeach()

# Explicit project GPU diagnostic surface, never inherited by an installed application.
target_include_directories(render_output_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude")
target_link_libraries(render_output_test PRIVATE NRI)
set_target_properties(render PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")

set(graph_shader_headers)
foreach(spec GraphFill.comp GraphTriangle.vert GraphMrt.frag GraphPresent.frag GraphCount.comp GraphRed.frag)
    get_filename_component(shader_name "${spec}" NAME_WE)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader_name}.hpp")
    set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/${spec}.spv")
    add_custom_command(
        OUTPUT "${header}"
        BYPRODUCTS "${binary}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${spec}" -o "${binary}"
        COMMAND lux_shader_metadata "${binary}" "${header}" "k${shader_name}"
        DEPENDS "shaders/${spec}" lux_shader_metadata
        VERBATIM
    )
    list(APPEND graph_shader_headers "${header}")
endforeach()

add_executable(render_graph_chain GraphChain.cpp ${graph_shader_headers})
target_include_directories(render_graph_chain PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders"
)
target_link_libraries(render_graph_chain PRIVATE render NRI)
set_target_properties(render_graph_chain PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin")
add_test(NAME render.graph.chain COMMAND render_graph_chain)
set_tests_properties(render.graph.chain PROPERTIES TIMEOUT 90)
