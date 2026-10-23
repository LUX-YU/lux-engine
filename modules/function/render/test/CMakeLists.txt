# Formal SDK consumers only. Mechanism/fault/installation/performance probes live in the external verification suite.
add_executable(render_sync_client SyncClient.cpp)
target_link_libraries(render_sync_client PRIVATE render lux::engine::platform::platform_ipc)
add_test(NAME render.client.sync_source COMMAND render_sync_client)


add_executable(render_output_test Output.cpp)
target_link_libraries(render_output_test PRIVATE render)
add_test(NAME render.native.output COMMAND render_output_test)
set_tests_properties(render.native.output PROPERTIES TIMEOUT 30)

get_filename_component(render_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
add_executable(render_material_asset_tool EmitMaterial.cpp)
target_link_libraries(render_material_asset_tool PRIVATE
    lux::engine::toolchain::toolchain_material_compiler
)
set(shader_root "${render_source_root}/modules/function/material/shaders")
set(shader "${CMAKE_CURRENT_BINARY_DIR}/assets/UnlitTexture.frag")
find_package(Vulkan REQUIRED COMPONENTS glslangValidator)
add_custom_command(
    OUTPUT "${shader}.spv" "${shader}.spv.meta"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets"
    COMMAND render_material_asset_tool "${shader}.spv" texture
    COMMAND lux_shader_metadata "${shader}.spv" "${shader}.spv.meta"
    DEPENDS render_material_asset_tool lux_shader_metadata "${shader_root}/material_types.glsl"
        "${shader_root}/texture_sampling.glsl"
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

# Explicit project GPU diagnostic surface, never inherited by an installed application.
target_include_directories(render_output_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude")
target_link_libraries(render_output_test PRIVATE NRI)

set(graph_shader_headers)
foreach(spec GraphFill.comp GraphTriangle.vert GraphMrt.frag GraphPresent.frag GraphCount.comp GraphRed.frag
    GraphHistory.comp GraphDepthField.comp GraphShadowSampler.frag ClusterCompare.comp
)
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
    "${CMAKE_CURRENT_BINARY_DIR}/../shaders"
)
target_link_libraries(render_graph_chain PRIVATE render NRI)
add_test(NAME render.graph.chain COMMAND render_graph_chain)
# The combined matrix creates 74 independent devices. ASan + validation exceeds 90 seconds;
# per-scenario progress assertions remain unchanged by this outer process budget.
set_tests_properties(render.graph.chain PROPERTIES TIMEOUT 180)

add_executable(render_deferred_scene DeferredScene.cpp)
target_include_directories(render_deferred_scene PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude")
target_link_libraries(render_deferred_scene PRIVATE render NRI lux::engine::core::core_execution
    lux::engine::toolchain::toolchain_material_compiler)
add_test(NAME render.sample.deferred COMMAND render_deferred_scene)
set_tests_properties(render.sample.deferred PROPERTIES TIMEOUT 90)

add_executable(render_shadow_scene ShadowScene.cpp ${graph_shader_headers})
target_include_directories(render_shadow_scene PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    "${CMAKE_CURRENT_BINARY_DIR}/../shaders"
)

target_link_libraries(render_shadow_scene PRIVATE
    render NRI lux::engine::core::core_execution
    lux::engine::toolchain::toolchain_material_compiler
)

add_test(NAME render.sample.shadow COMMAND render_shadow_scene)
set_tests_properties(render.sample.shadow PROPERTIES TIMEOUT 120)

add_test(NAME render.sample.clustered_shadow COMMAND render_shadow_scene 3 --clustered)
set_tests_properties(render.sample.clustered_shadow PROPERTIES TIMEOUT 120)

add_executable(render_clustered_scene ClusteredScene.cpp ${graph_shader_headers})
target_include_directories(render_clustered_scene PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    "${CMAKE_CURRENT_BINARY_DIR}/../shaders"
)

target_link_libraries(render_clustered_scene PRIVATE
    render NRI lux::engine::core::core_execution
    lux::engine::toolchain::toolchain_material_compiler
)

add_test(NAME render.sample.clustered COMMAND render_clustered_scene)
set_tests_properties(render.sample.clustered PROPERTIES TIMEOUT 120)

# Render Command contracts live with Render; Platform IPC remains independently buildable.
set(ipc_test_source "${CMAKE_CURRENT_SOURCE_DIR}/../../../platform/ipc/test")
find_package(lux-cxx CONFIG REQUIRED COMPONENTS memory)
add_executable(render_ipc_commands_test IpcCommands.cpp "${ipc_test_source}/Allocation.cpp")
target_include_directories(render_ipc_commands_test PRIVATE "${ipc_test_source}" "${ipc_test_source}/../pinclude")
target_link_libraries(render_ipc_commands_test PRIVATE render lux::engine::platform::platform_ipc lux::cxx::memory)
# Preserve the original contract name; the cost case is now distinct from the raw IPC queue measurement.
add_test(NAME platform.ipc.protocol COMMAND render_ipc_commands_test protocol)
add_test(NAME render.ipc.command_cost COMMAND render_ipc_commands_test cost)
set_tests_properties(platform.ipc.protocol render.ipc.command_cost PROPERTIES TIMEOUT 90)
