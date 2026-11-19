add_executable(render_output_test Output.cpp)

if(WIN32)
    target_link_libraries(render_output_test PRIVATE Vulkan::Headers)
endif()

add_test(NAME render.native.output COMMAND render_output_test)
set_tests_properties(render.native.output PROPERTIES TIMEOUT 30)

add_test(NAME render.native.device_lost_completion COMMAND render_output_test --device-lost)
set_tests_properties(render.native.device_lost_completion PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

set(graph_shader_headers)
foreach(spec GraphFill.comp GraphTriangle.vert GraphMrt.frag GraphPresent.frag GraphCount.comp GraphRed.frag
    GraphHistory.comp GraphDepthField.comp GraphShadowSampler.frag ClusterCompare.comp GraphConstantLayout.comp
)
    get_filename_component(shader_name "${spec}" NAME_WE)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader_name}.hpp")
    get_filename_component(extension "${spec}" EXT)
    if(extension STREQUAL ".comp")
        set(stage compute)
    elseif(extension STREQUAL ".vert")
        set(stage vertex)
    else()
        set(stage fragment)
    endif()
    lux_render_shader("${header}" "shaders/${shader_name}.slang" ${stage} "k${shader_name}")
    list(APPEND graph_shader_headers "${header}")
endforeach()

add_executable(render_graph_chain GraphChain.cpp ${graph_shader_headers})
target_include_directories(render_graph_chain PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    "${CMAKE_CURRENT_BINARY_DIR}/../shaders"
)
add_test(NAME render.graph.chain COMMAND render_graph_chain)
# The combined matrix creates 74 independent devices. ASan + validation exceeds 90 seconds;
# per-scenario progress assertions remain unchanged by this outer process budget.
set_tests_properties(render.graph.chain PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
if(WIN32)
    add_test(NAME render.graph.chain_d3d12 COMMAND render_graph_chain chain d3d12)
    set_tests_properties(render.graph.chain_d3d12 PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
endif()

add_test(NAME render.graph.analysis COMMAND render_graph_chain analysis)
add_test(NAME render.graph.cpu_recording COMMAND render_graph_chain cpu-recording)
if(WIN32 AND LUX_RENDER_GRAPH_ALLOCATION_PROBE)
    target_sources(render_graph_chain PRIVATE GraphMeasurements.cpp)
    target_compile_definitions(render_graph_chain PRIVATE LUX_RENDER_GRAPH_ALLOCATION_PROBE)
    add_test(NAME render.graph.measure_vulkan COMMAND render_graph_chain measure)
    add_test(NAME render.graph.measure_d3d12 COMMAND render_graph_chain measure d3d12)
    set_tests_properties(render.graph.measure_vulkan render.graph.measure_d3d12 PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()

add_test(NAME render.graph.typed_vulkan COMMAND render_graph_chain encoding)
add_test(NAME render.graph.recording_vulkan COMMAND render_graph_chain recording)
add_test(NAME render.graph.raster_vulkan COMMAND render_graph_chain raster)
if(WIN32)
    add_test(NAME render.graph.typed_d3d12 COMMAND render_graph_chain encoding d3d12)
    add_test(NAME render.graph.recording_d3d12 COMMAND render_graph_chain recording d3d12)
    add_test(NAME render.graph.raster_d3d12 COMMAND render_graph_chain raster d3d12)
endif()

add_executable(render_deferred_scene DeferredScene.cpp)

target_link_libraries(render_deferred_scene PRIVATE
    lux::engine::function::material_compiler lux::engine::function::material_graph
)

# The GPU probes instantiate the production native host through its private header.
add_library(render_native_probe_host STATIC "${CMAKE_CURRENT_SOURCE_DIR}/../src/device/NativeExecution.cpp")

target_include_directories(render_native_probe_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../pinclude")

target_link_libraries(render_native_probe_host PUBLIC render_extensions)

foreach(probe IN ITEMS render_output_test render_graph_chain render_deferred_scene)
    target_include_directories(${probe} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude")

    target_link_libraries(${probe} PRIVATE render_native_probe_host NRI)
endforeach()

add_test(NAME render.sample.deferred COMMAND render_deferred_scene)
set_tests_properties(render.sample.deferred PROPERTIES TIMEOUT 90)

set(ipc_test_source "${CMAKE_CURRENT_SOURCE_DIR}/../../../platform/ipc/test")

if(LUX_RENDER_DEVELOPMENT_METRICS)
    add_executable(render_material_scale MaterialScale.cpp)
    target_include_directories(render_material_scale PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude")
    if(WIN32)
        target_sources(render_material_scale PRIVATE "${ipc_test_source}/Allocation.cpp")
        target_include_directories(render_material_scale PRIVATE "${ipc_test_source}")
    endif()
    target_link_libraries(render_material_scale PRIVATE
        render_native_probe_host NRI lux::engine::function::material_compiler lux::engine::function::material_graph
        lux::engine::core::math
    )
    add_test(NAME render.material.purposes_vulkan COMMAND render_material_scale)
    add_test(NAME render.material.pipeline_isolation_vulkan COMMAND render_material_scale --pipeline-isolation)
    add_test(NAME render.material.pipeline_shutdown_vulkan COMMAND render_material_scale --pipeline-shutdown)
    add_test(
        NAME render.material.pipeline_single_worker
        COMMAND render_material_scale --pipeline-isolation --single-worker
    )
    if(WIN32)
        add_test(NAME render.material.purposes_d3d12 COMMAND render_material_scale d3d12)
        add_test(NAME render.material.pipeline_isolation_d3d12 COMMAND render_material_scale --pipeline-isolation d3d12)
        add_test(NAME render.material.pipeline_shutdown_d3d12 COMMAND render_material_scale --pipeline-shutdown d3d12)
    endif()
endif()

add_executable(render_shadow_scene ShadowScene.cpp ${graph_shader_headers})
target_include_directories(render_shadow_scene PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../sinclude"
    "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    "${CMAKE_CURRENT_BINARY_DIR}/../shaders"
)

target_link_libraries(render_shadow_scene PRIVATE
    render NRI lux::engine::core::core_execution
    lux::engine::function::material_compiler lux::engine::function::material_graph
)

add_test(NAME render.sample.shadow COMMAND render_shadow_scene)
target_link_libraries(render_shadow_scene PRIVATE render_extensions)
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
    lux::engine::function::material_compiler lux::engine::function::material_graph
)

add_test(NAME render.sample.clustered COMMAND render_clustered_scene)
target_link_libraries(render_clustered_scene PRIVATE render_extensions)
set_tests_properties(render.sample.clustered PROPERTIES TIMEOUT 120)

# Render Command contracts live with Render; Platform IPC remains independently buildable.
find_package(lux-cxx CONFIG REQUIRED COMPONENTS memory)
add_executable(render_ipc_commands_test IpcCommands.cpp "${ipc_test_source}/Allocation.cpp")
target_include_directories(render_ipc_commands_test PRIVATE "${ipc_test_source}" "${ipc_test_source}/../pinclude")
target_link_libraries(render_ipc_commands_test PRIVATE
    render lux::engine::platform::platform_ipc lux::cxx::memory
)
add_test(NAME platform.ipc.protocol COMMAND render_ipc_commands_test)
set_tests_properties(platform.ipc.protocol PROPERTIES TIMEOUT 30)

# Only fixtures constructing rdesc::Mesh require its Math dependency. Command-only Clients stay neutral.
foreach(consumer IN ITEMS render_deferred_scene render_shadow_scene render_clustered_scene)
    target_link_libraries(${consumer} PRIVATE lux::engine::core::math)
endforeach()
