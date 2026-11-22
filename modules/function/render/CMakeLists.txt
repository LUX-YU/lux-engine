find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core execution arguments)

find_package(stdexec CONFIG REQUIRED GLOBAL)

find_package(Eigen3 CONFIG REQUIRED)

if(NOT TARGET lux::engine::core::math)
    find_package(lux-engine-core CONFIG REQUIRED COMPONENTS math)
endif()

if(NOT TARGET lux::engine::core::serialization)
    find_package(lux-engine CONFIG REQUIRED COMPONENTS core::serialization)
endif()

if(NOT TARGET lux::engine::function::graph)
    find_package(lux-engine-function CONFIG REQUIRED COMPONENTS graph)
endif()

find_package(Vulkan 1.3 REQUIRED)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../core/error/cmake/Values.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../resource/description/cmake/ValueTypes.cmake")
include(cmake/ShaderTools.cmake)
include(cmake/Nri.cmake)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_RENDER_LIBRARY
    PUBLIC_MACRO_NAME LUX_RENDER_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_RENDER_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/render/visibility.h
)

set(render_sources
    src/effect/DepthPyramid.cpp
    src/effect/MeshVisibility.cpp
    src/effect/MeshSurfaceStream.cpp
    src/effect/EffectCatalog.cpp
    src/effect/EffectGraphBuild.cpp
    src/effect/UniformBlockVersions.cpp
    src/effect/EffectGraph.cpp
    src/effect/EffectValidation.cpp
    src/effect/Lighting.cpp
    src/effect/ToneMapping.cpp
    src/device/CopyEncoding.cpp
    src/runtime/RenderClient.cpp
    src/frame/FrameLayout.cpp
    src/frame/FrameData.cpp
    src/resource/transform/TransformStorage.cpp
    src/resource/transform/TransformReplicas.cpp
    src/resource/view/ViewParameterStorage.cpp
    src/core/Target.cpp
    src/core/Error.cpp
    src/core/RasterViewport.cpp
    src/commands/Command.cpp
    src/resource/ResourceDefinition.cpp
    src/resource/StorageMeta.cpp
    src/resource/GraphInstallation.cpp
    src/resource/ResourceRegistry.cpp
    src/resource/FinalBufferVersions.cpp
    src/resource/ResourceSet.cpp
    src/resource/mesh/MeshPacking.cpp
    src/resource/mesh/IndexedTriangleMesh.cpp
    src/resource/material/RasterMaterial.cpp
    src/graph/RenderGraphBuilder.cpp
    src/graph/PassNormalizer.cpp
    src/graph/compile/GraphAnalysis.cpp
    src/graph/Diagnostics.cpp
    src/features/mesh/RasterMesh.cpp
    src/features/lighting/ClusteredLightingWork.cpp
    src/features/shadow/ShadowProjection.cpp
    src/features/mesh/MeshVisibilityWork.cpp
    src/features/common/FullscreenRaster.cpp
    src/resource/lighting/DirectLightingStorage.cpp
    src/features/mesh/MeshGrouping.cpp
    src/device/Error.cpp
    src/device/NativeExecution.cpp
    src/device/JobGpuResources.cpp
    src/device/DeviceSession.cpp
    src/device/DescriptorTable.cpp
    src/device/DeviceBackend.cpp
    src/device/d3d12/DeviceBackend.cpp
    src/device/SubmissionQueue.cpp
    src/graph/execute/FrameResources.cpp
    src/graph/execute/ResizeResources.cpp
    src/graph/execute/HistoryState.cpp
    src/target/OffscreenTarget.cpp
    src/target/SwapchainTarget.cpp
    src/graph/execute/RenderGraphInstance.cpp
    src/graph/execute/RenderGraphInstanceProducers.cpp
    src/graph/execute/RenderGraphInstanceRasterStreams.cpp
    src/graph/execute/RenderGraphInstanceResources.cpp
    src/graph/execute/RenderGraphInstanceDescriptors.cpp
    src/graph/execute/TransferFreeze.cpp
    src/graph/execute/RenderGraphInstanceRecording.cpp
    src/graph/execute/FrameExecutor.cpp
    src/graph/execute/NriRecordCommands.cpp
    src/graph/compile/RenderGraphExecutable.cpp
    src/device/CompiledPrograms.cpp
    src/device/RasterProgramUpdate.cpp
    src/resource/ResourceProgramUses.cpp
    src/graph/PlanDiagnostics.cpp
    src/graph/compile/BuiltinPass.cpp
    src/graph/compile/GraphCompiler.cpp
    src/graph/compile/EffectCompiler.cpp
    src/graph/compile/RecordingDependencies.cpp
    src/graph/compile/RenderingPlan.cpp
    src/graph/compile/ConditionalPlan.cpp
    src/graph/compile/SubmissionPlan.cpp
    src/graph/compile/AliasUseFrontier.cpp
    src/graph/compile/PhysicalResourcePlan.cpp
    src/graph/compile/BarrierPlan.cpp
    src/graph/compile/ShaderBindingPlan.cpp
    src/graph/Shader.cpp
    src/graph/RasterProgram.cpp
    src/graph/ShaderInterface.cpp
    src/upload/ResourceUploader.cpp
    src/upload/StagingArena.cpp
    src/device/BufferArena.cpp
    src/resource/texture/SampledTexture2DStorage.cpp
    src/resource/material/RasterMaterialStorage.cpp
    src/resource/instance/RasterMeshInstanceStorage.cpp
    src/resource/mesh/IndexedTriangleMeshStorage.cpp
    src/runtime/RenderServer.cpp
    src/commands/CommandRegistry.cpp
    src/runtime/RenderServerImpl.cpp
    src/resource/ResourceScopes.cpp
    src/runtime/FrameProgress.cpp
    src/runtime/FrameOperation.cpp
    src/runtime/SubmissionOperation.cpp
    src/runtime/JobControl.cpp
    src/runtime/JobRelations.cpp
    src/runtime/JobResourceBinding.cpp
    src/runtime/JobExecution.cpp
    src/runtime/RenderJob.cpp
    src/runtime/DeviceEntry.cpp
    src/runtime/Scene.cpp
    src/runtime/RenderEffect.cpp
    src/commands/CommandDispatchContext.cpp
    src/runtime/DeviceRound.cpp
    src/runtime/FrameInputs.cpp
    src/commands/ResponseOutbox.cpp
)

include(cmake/MeshShaderVariants.cmake)
lux_render_mesh_shaders(mesh_shader_headers)

set(hzb_header "${CMAKE_CURRENT_BINARY_DIR}/shaders/HzbDownsample.hpp")
lux_render_shader("${hzb_header}" shaders/hzb/Downsample.slang compute kHzbDownsample)

set(material_shader_root "${CMAKE_CURRENT_SOURCE_DIR}/../material/shaders")
set(feature_shader_headers)
foreach(shader
    FullscreenVertex
    DeferredLighting DeferredLightingShadow
    DeferredLightingClustered DeferredLightingClusteredShadow
    TonemapLinear TonemapSrgb
)
    set(defines)
    if(shader STREQUAL "FullscreenVertex")
        set(source shaders/common/FullscreenVertex.slang)
    elseif(shader MATCHES "^DeferredLighting")
        set(source shaders/deferred/DeferredLightingFragment.slang)
        if(shader MATCHES "Shadow$")
            list(APPEND defines -DSHADOWS=1)
        endif()
        if(shader MATCHES "Clustered")
            list(APPEND defines -DCLUSTERED=1)
        endif()
    else()
        set(source shaders/postprocess/TonemapFragment.slang)
        if(shader STREQUAL "TonemapSrgb")
            list(APPEND defines -DOUTPUT_SRGB=1)
        endif()
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.hpp")
    set(shader_stage fragment)
    if(shader MATCHES "Vertex$")
        set(shader_stage vertex)
    endif()
    lux_render_shader("${header}" "${source}" ${shader_stage} k${shader} ${defines})
    list(APPEND feature_shader_headers "${header}")
endforeach()

foreach(shader MeshFrustumCull MeshFrustumCullPaged MeshCompact MeshReset MeshView)
    set(defines)
    if(shader STREQUAL "MeshFrustumCullPaged")
        set(source shaders/mesh/MeshFrustumCull.slang)
        list(APPEND defines -DPAGED_INSTANCES=1)
    else()
        set(source "shaders/mesh/${shader}.slang")
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.hpp")
    set(target_options)
    if(shader MATCHES "Paged$")
        list(APPEND target_options --physical-pointers)
    endif()
    lux_render_shader("${header}" "${source}" compute k${shader} ${defines} ${target_options})
    list(APPEND feature_shader_headers "${header}")
endforeach()

foreach(stage build count scan fill)
    string(SUBSTRING "${stage}" 0 1 first)
    string(TOUPPER "${first}" first)
    string(SUBSTRING "${stage}" 1 -1 rest)
    set(shader "Cluster${first}${rest}")
    set(source "shaders/deferred/cluster_${stage}.slang")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.hpp")
    lux_render_shader("${header}" "${source}" compute k${shader})
    list(APPEND feature_shader_headers "${header}")
endforeach()

add_component(
    COMPONENT_NAME render
    NAMESPACE lux::engine::function
    OUTPUT_NAME lux_engine_render
    SOURCE_FILES ${render_sources} ${mesh_shader_headers} ${feature_shader_headers} "${hzb_header}"
)

component_include_directories(
    render
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}"
    INSTALL_TIME include
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sinclude" "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
        "${CMAKE_CURRENT_BINARY_DIR}/shaders" "${LUX_NRI_VMA_HEADERS}" "${LUX_NRI_VULKAN_HEADERS}"
)

add_interface_component(
    COMPONENT_NAME render_extensions
    NAMESPACE lux::engine::function
)

component_include_directories(
    render_extensions
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}"
    INSTALL_TIME include
)

# Authors borrow the existing runtime/backend. This target compiles no Render implementation.
target_link_libraries(render_extensions INTERFACE render STDEXEC::stdexec lux::cxx::execution lux::engine::core::core_execution)

target_include_directories(render PRIVATE
    "${LUX_NRI_SOURCE_DIR}/Include"
)

target_include_directories(render_extensions INTERFACE
    "$<BUILD_INTERFACE:${LUX_NRI_SOURCE_DIR}/Include>"
    "$<INSTALL_INTERFACE:include/lux/engine/render/nri>"
)

install(DIRECTORY "${LUX_NRI_SOURCE_DIR}/Include/"
    DESTINATION include/lux/engine/render/nri
    COMPONENT lux_engine_render
    FILES_MATCHING PATTERN "*.h"
)

if(WIN32)
    set(agility_runtime "${LUX_RENDER_DEPENDENCY_CACHE}/nri-v192-content/agilitysdk-src/build/native/bin/x64")
    set(agility_libraries "${agility_runtime}/D3D12Core.dll" "${agility_runtime}/d3d12SDKLayers.dll")
    add_custom_command(
        TARGET render POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:render>/AgilitySDK"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${agility_libraries} "$<TARGET_FILE_DIR:render>/AgilitySDK"
        VERBATIM
    )
    install(FILES ${agility_libraries} DESTINATION bin/AgilitySDK COMPONENT lux_engine_render)
    install(FILES
        "${LUX_RENDER_DEPENDENCY_CACHE}/nri-v192-content/agilitysdk-src/LICENSE.txt"
        "${LUX_RENDER_DEPENDENCY_CACHE}/nri-v192-content/agilitysdk-src/LICENSE-CODE.txt"
        DESTINATION share/lux-engine-render/licenses/AgilitySDK
        COMPONENT lux_engine_render
    )
endif()

target_compile_definitions(render PRIVATE LUX_RENDER_LIBRARY)

option(LUX_RENDER_DEVELOPMENT_METRICS "Collect Render development metrics" OFF)
if(LUX_RENDER_DEVELOPMENT_METRICS)
    target_compile_definitions(render PRIVATE LUX_RENDER_DEVELOPMENT_METRICS)
endif()
option(LUX_RENDER_GRAPH_ALLOCATION_PROBE "Count C++ allocations in Render graph probes on Windows" OFF)
if(WIN32 AND LUX_RENDER_GRAPH_ALLOCATION_PROBE)
    set(ipc_allocation_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/ipc/test")
    target_sources(render PRIVATE test/GraphAllocation.cpp "${ipc_allocation_dir}/Allocation.cpp")
    target_include_directories(render PRIVATE "${ipc_allocation_dir}")
endif()

target_compile_features(render PUBLIC cxx_std_20)
target_link_libraries(render
    PUBLIC lux::cxx::container lux::cxx::compile_time lux::cxx::algorithm lux::cxx::core
        lux::engine::core::error_values lux::engine::resource::description_types
        lux::engine::resource::shader_metadata lux::engine::platform::platform_ipc
        STDEXEC::stdexec lux::cxx::execution lux::engine::core::core_execution
        lux::engine::function::graph
    PRIVATE NRI Eigen3::Eigen lux::engine::core::math lux::cxx::arguments
        lux::engine::core::serialization
        spirv-cross-c SPIRV-Tools-static
        lux::engine::platform::platform_process
)

component_add_transitive_commands(render
    "find_package(stdexec CONFIG REQUIRED GLOBAL)"
    "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core execution)"
    "find_package(lux-engine-core CONFIG REQUIRED COMPONENTS error_values)"
    "find_package(lux-engine-function CONFIG REQUIRED COMPONENTS graph)"
    "find_package(lux-engine-resource CONFIG REQUIRED COMPONENTS description_types shader_metadata)"
    "find_package(lux-engine-platform CONFIG REQUIRED COMPONENTS platform_ipc)"
)

component_add_transitive_commands(render_extensions
    "find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render)"
    "find_package(stdexec CONFIG REQUIRED GLOBAL)"
    "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-render
    VERSION 0.0.1
    NAMESPACE lux::engine::function
    COMPONENTS render render_extensions
)

if(BUILD_TESTING OR LUX_RENDER_BUILD_PROBES)
    add_subdirectory(test)
endif()
