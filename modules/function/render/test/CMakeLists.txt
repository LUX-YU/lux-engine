add_executable(render_effect_graph_test EffectGraph.cpp)

target_include_directories(render_effect_graph_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pinclude")

target_link_libraries(render_effect_graph_test PRIVATE render)

add_test(NAME render.effect.graph COMMAND render_effect_graph_test)

set(effect_graph_shader "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectGraphColor.hpp")
lux_render_shader("${effect_graph_shader}" "shaders/EffectGraphColor.slang" compute kEffectGraphColor)

set(effect_import_shader "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectGraphImport.hpp")
lux_render_shader("${effect_import_shader}" "shaders/EffectGraphImport.slang" compute kEffectGraphImport)

set(effect_uniform_shader "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectNodeUniform.hpp")
lux_render_shader("${effect_uniform_shader}" "shaders/EffectNodeUniform.slang" compute kEffectNodeUniform)

set(effect_producer_shader "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectGraphProducer.hpp")
lux_render_shader("${effect_producer_shader}" "shaders/EffectGraphProducer.slang" compute kEffectGraphProducer)

set(effect_prepared_buffer "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectPreparedBuffer.hpp")
lux_render_shader("${effect_prepared_buffer}" "shaders/EffectPreparedBuffer.slang" compute kEffectPreparedBuffer)

set(effect_stream_vertex "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectStreamVertex.hpp")
set(effect_stream_inline "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectStreamInline.hpp")
set(effect_stream_uniform "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectStreamUniform.hpp")
set(effect_stream_layers "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectStreamLayers.hpp")
set(effect_stream_indirect "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectStreamIndirect.hpp")
set(effect_stream_shared_texture "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectStreamSharedTexture.hpp")
set(effect_stream_mesh "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectStreamMesh.hpp")
set(effect_stream_material "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectStreamMaterial.hpp")
set(effect_mesh_imports "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectMeshImports.hpp")
set(effect_mesh_imports_paged "${CMAKE_CURRENT_BINARY_DIR}/shaders/EffectMeshImportsPaged.hpp")

lux_render_shader("${effect_stream_vertex}" "shaders/EffectStreamVertex.slang" vertex kEffectStreamVertex)
lux_render_shader("${effect_stream_mesh}" "shaders/EffectStreamMesh.slang" vertex kEffectStreamMesh)
lux_render_shader("${effect_stream_material}" "shaders/EffectStreamMaterial.slang" fragment kEffectStreamMaterial)
lux_render_shader("${effect_mesh_imports}" "shaders/EffectMeshImports.slang" compute kEffectMeshImports)
lux_render_shader(
    "${effect_mesh_imports_paged}" "shaders/EffectMeshImports.slang" compute kEffectMeshImportsPaged
    -DPAGED_INSTANCES=1 --physical-pointers
)
lux_render_shader("${effect_stream_inline}" "shaders/EffectStreamInline.slang" fragment kEffectStreamInline)
lux_render_shader("${effect_stream_uniform}" "shaders/EffectStreamUniform.slang" fragment kEffectStreamUniform)
lux_render_shader("${effect_stream_layers}" "shaders/EffectStreamLayers.slang" compute kEffectStreamLayers)
lux_render_shader("${effect_stream_indirect}" "shaders/EffectStreamIndirect.slang" compute kEffectStreamIndirect)
lux_render_shader(
    "${effect_stream_shared_texture}" "shaders/EffectStreamSharedTexture.slang" fragment kEffectStreamSharedTexture
)

add_executable(render_output_test
    Output.cpp
    "${effect_graph_shader}"
    "${effect_import_shader}"
    "${effect_producer_shader}"
    "${effect_prepared_buffer}"
    "${effect_uniform_shader}"
    "${effect_stream_vertex}"
    "${effect_stream_inline}"
    "${effect_stream_uniform}"
    "${effect_stream_layers}"
    "${effect_stream_indirect}"
    "${effect_stream_shared_texture}"
    "${effect_stream_mesh}"
    "${effect_stream_material}"
    "${effect_mesh_imports}"
    "${effect_mesh_imports_paged}"
)

target_include_directories(render_output_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")

if(WIN32)
    target_link_libraries(render_output_test PRIVATE Vulkan::Headers)
endif()

add_test(NAME render.native.output COMMAND render_output_test)
set_tests_properties(render.native.output PROPERTIES TIMEOUT 30)

add_test(NAME render.effect.output_vulkan COMMAND render_output_test --effect-graph)
set_tests_properties(render.effect.output_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_test(NAME render.effect.uniform_queues_vulkan COMMAND render_output_test --effect-graph vulkan async-uniform)
set_tests_properties(render.effect.uniform_queues_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

add_test(NAME render.effect.uniform_faults_vulkan COMMAND render_output_test --effect-graph vulkan uniform-faults)
set_tests_properties(render.effect.uniform_faults_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

add_test(NAME render.effect.depth_pyramid_vulkan COMMAND render_output_test --basic-features)
set_tests_properties(render.effect.depth_pyramid_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_test(NAME render.effect.lighting_vulkan COMMAND render_output_test --lighting-graph)
set_tests_properties(render.effect.lighting_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_test(NAME render.effect.prepared_buffer_vulkan COMMAND render_output_test --prepared-buffer)
set_tests_properties(render.effect.prepared_buffer_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_test(NAME render.effect.shared_texture_vulkan COMMAND render_output_test --shared-texture)
set_tests_properties(render.effect.shared_texture_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

add_test(NAME render.effect.mesh_stream_vulkan COMMAND render_output_test --mesh-stream)
set_tests_properties(render.effect.mesh_stream_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_test(NAME render.effect.mesh_imports_vulkan COMMAND render_output_test --mesh-imports)
set_tests_properties(render.effect.mesh_imports_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_test(NAME render.effect.mesh_imports_paged COMMAND render_output_test --mesh-imports paged)
set_tests_properties(render.effect.mesh_imports_paged PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

add_test(NAME render.effect.mesh_visibility_vulkan COMMAND render_output_test --mesh-visibility)
set_tests_properties(render.effect.mesh_visibility_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_test(NAME render.effect.mesh_visibility_paged COMMAND render_output_test --mesh-visibility paged)
set_tests_properties(render.effect.mesh_visibility_paged PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

add_test(NAME render.effect.gbuffer_visibility_vulkan COMMAND render_output_test --gbuffer-visibility)
set_tests_properties(render.effect.gbuffer_visibility_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_test(NAME render.effect.gbuffer_visibility_paged COMMAND render_output_test --gbuffer-visibility paged)
set_tests_properties(render.effect.gbuffer_visibility_paged PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

add_test(NAME render.effect.material_stream_vulkan COMMAND render_output_test --material-stream)
set_tests_properties(render.effect.material_stream_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

if(WIN32)
    foreach(backend IN ITEMS vulkan d3d12)
        add_test(NAME render.upload.acquire_failure_${backend}
            COMMAND render_output_test --upload-acquire-failure ${backend}
        )
        set_tests_properties(render.upload.acquire_failure_${backend}
            PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77
        )
    endforeach()

    add_test(NAME render.effect.output_d3d12 COMMAND render_output_test --effect-graph d3d12)
    set_tests_properties(render.effect.output_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.effect.uniform_queues_d3d12 COMMAND render_output_test --effect-graph d3d12 async-uniform)
    set_tests_properties(render.effect.uniform_queues_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

    add_test(NAME render.effect.uniform_faults_d3d12 COMMAND render_output_test --effect-graph d3d12 uniform-faults)
    set_tests_properties(render.effect.uniform_faults_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

    add_test(NAME render.effect.depth_pyramid_d3d12 COMMAND render_output_test --basic-features d3d12)
    set_tests_properties(render.effect.depth_pyramid_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.effect.lighting_d3d12 COMMAND render_output_test --lighting-graph d3d12)
    set_tests_properties(render.effect.lighting_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.effect.prepared_buffer_d3d12 COMMAND render_output_test --prepared-buffer d3d12)
    set_tests_properties(render.effect.prepared_buffer_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.effect.shared_texture_d3d12 COMMAND render_output_test --shared-texture d3d12)
    set_tests_properties(render.effect.shared_texture_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)

    add_test(NAME render.effect.mesh_stream_d3d12 COMMAND render_output_test --mesh-stream d3d12)
    set_tests_properties(render.effect.mesh_stream_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.effect.mesh_imports_d3d12 COMMAND render_output_test --mesh-imports d3d12)
    set_tests_properties(render.effect.mesh_imports_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.effect.mesh_visibility_d3d12 COMMAND render_output_test --mesh-visibility d3d12)
    set_tests_properties(render.effect.mesh_visibility_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.effect.gbuffer_visibility_d3d12 COMMAND render_output_test --gbuffer-visibility d3d12)
    set_tests_properties(render.effect.gbuffer_visibility_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.effect.material_stream_d3d12 COMMAND render_output_test --material-stream d3d12)
    set_tests_properties(render.effect.material_stream_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
endif()

add_test(NAME render.native.device_lost_completion COMMAND render_output_test --device-lost)
set_tests_properties(render.native.device_lost_completion PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

set(graph_shader_headers)
foreach(spec GraphFill.comp GraphTriangle.vert GraphMrt.frag GraphPresent.frag GraphCount.comp GraphRed.frag
    GraphHistory.comp GraphDepthField.comp GraphShadowSampler.frag ClusterCompare.comp GraphConstantLayout.comp
    ShadowReadback.comp GraphTableSize.frag
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

add_test(NAME render.graph.local_freeze_vulkan COMMAND render_graph_chain local-freeze vulkan)
set_tests_properties(render.graph.local_freeze_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

add_test(NAME render.graph.batch_reuse_vulkan COMMAND render_graph_chain batch-reuse vulkan)
set_tests_properties(render.graph.batch_reuse_vulkan PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

if(WIN32)
    add_test(NAME render.graph.local_freeze_d3d12 COMMAND render_graph_chain local-freeze d3d12)
    set_tests_properties(render.graph.local_freeze_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.graph.batch_reuse_d3d12 COMMAND render_graph_chain batch-reuse d3d12)
    set_tests_properties(render.graph.batch_reuse_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)
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

add_test(NAME render.effect.gbuffer_vulkan COMMAND render_deferred_scene 3 --effect-graph)
set_tests_properties(render.effect.gbuffer_vulkan PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
add_test(NAME render.effect.gbuffer_paged COMMAND render_deferred_scene 3 --effect-graph --paged)
set_tests_properties(render.effect.gbuffer_paged PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
add_test(NAME render.effect.shadow_vulkan COMMAND render_deferred_scene 3 --effect-shadow)
set_tests_properties(render.effect.shadow_vulkan PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
add_test(NAME render.effect.shadow_paged COMMAND render_deferred_scene 3 --effect-shadow --paged)
set_tests_properties(render.effect.shadow_paged PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
if(WIN32)
    add_test(NAME render.effect.gbuffer_d3d12 COMMAND render_deferred_scene 3 --effect-graph --d3d12)
    set_tests_properties(render.effect.gbuffer_d3d12 PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
    add_test(NAME render.effect.shadow_d3d12 COMMAND render_deferred_scene 3 --effect-shadow --d3d12)
    set_tests_properties(render.effect.shadow_d3d12 PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endif()

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
add_test(NAME render.sample.clustered_single_queue COMMAND render_clustered_scene 1 --single-queue)
add_test(NAME render.sample.clustered_same_family COMMAND render_clustered_scene 2 --same-family)
add_test(NAME render.sample.clustered_paged COMMAND render_clustered_scene 3 --paged)

if(WIN32)
    add_test(NAME render.native.descriptor_snapshots_d3d12 COMMAND render_deferred_scene --descriptor-snapshots)
    set_tests_properties(render.native.descriptor_snapshots_d3d12 PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE)

    add_test(NAME render.sample.clustered_d3d12 COMMAND render_clustered_scene 3 --d3d12)
    set_tests_properties(render.sample.clustered_d3d12 PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

target_link_libraries(render_clustered_scene PRIVATE render_extensions)
set_tests_properties(
    render.sample.clustered render.sample.clustered_single_queue
    render.sample.clustered_same_family render.sample.clustered_paged
    PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
)

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
foreach(consumer IN ITEMS render_output_test render_deferred_scene render_shadow_scene render_clustered_scene)
    target_link_libraries(${consumer} PRIVATE lux::engine::core::math)
endforeach()
