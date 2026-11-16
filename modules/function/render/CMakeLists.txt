find_package(imgui REQUIRED COMPONENTS core)

find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core execution)

find_package(stdexec CONFIG REQUIRED GLOBAL)

find_package(Eigen3 CONFIG REQUIRED)

if(NOT TARGET lux::engine::core::math)
    find_package(lux-engine-core CONFIG REQUIRED COMPONENTS math)
endif()

if(NOT TARGET lux::engine::core::serialization)
    find_package(lux-engine CONFIG REQUIRED COMPONENTS core::serialization)
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
    src/runtime/RenderClient.cpp
    src/frame/FrameStorage.cpp
    src/resource/view/CameraInputHandler.cpp
    src/resource/ui/UiInputHandler.cpp
    src/core/Target.cpp
    src/core/Error.cpp
    src/core/RasterViewport.cpp
    src/commands/Command.cpp
    src/extensions/features/CapabilityGraph.cpp
    src/capabilities/CapabilityComposition.cpp
    src/capabilities/RasterSurface.cpp
    src/capabilities/DeferredLighting.cpp
    src/capabilities/MeshShadows.cpp
    src/capabilities/RenderPresets.cpp
    src/resource/ResourceDefinition.cpp
    src/resource/StorageMeta.cpp
    src/resource/ResourceRegistry.cpp
    src/resource/ResourceSet.cpp
    src/resource/mesh/MeshPacking.cpp
    src/resource/mesh/IndexedTriangleMesh.cpp
    src/resource/material/RasterMaterial.cpp
    src/graph/RenderGraphBuilder.cpp
    src/graph/RenderGraphAnalysis.cpp
    src/graph/Diagnostics.cpp
    src/features/FeatureAssembly.cpp
    src/features/FeatureBuildContext.cpp
    src/resource/view/CameraParameters.cpp
    src/features/RasterMeshParameters.cpp
    src/features/RasterMesh.cpp
    src/features/MaterialMeshRaster.cpp
    src/features/MeshDepthRaster.cpp
    src/features/HzbFeature.cpp
    src/features/GBufferFeature.cpp
    src/features/DeferredLightingFeature.cpp
    src/features/ClusteredLightingFeature.cpp
    src/features/ShadowMapFeature.cpp
    src/features/ShadowProjection.cpp
    src/features/ShadowProjectionBinding.cpp
    src/features/MeshVisibilityFeature.cpp
    src/features/MeshShadowFeature.cpp
    src/features/TonemapFeature.cpp
    src/features/UiRasterFeature.cpp
    src/features/UiScene.cpp
    src/features/FullscreenRaster.cpp
    src/features/ParameterLayout.cpp
    src/resource/lighting/DirectLightingStorage.cpp
    src/features/RasterMeshDraws.cpp
    src/features/MeshDrawGroups.cpp
    src/device/Error.cpp
    src/device/NativeExecution.cpp
    src/device/DeviceSession.cpp
    src/device/DescriptorTable.cpp
    src/device/DeviceBackend.cpp
    src/device/d3d12/DeviceBackend.cpp
    src/device/SubmissionQueue.cpp
    src/execution/FrameExecution.cpp
    src/execution/FrameResources.cpp
    src/execution/ResizeResources.cpp
    src/execution/HistoryState.cpp
    src/execution/OffscreenTarget.cpp
    src/execution/SurfaceTarget.cpp
    src/execution/RenderGraphInstance.cpp
    src/execution/RenderExecutor.cpp
    src/graph/RenderPlan.cpp
    src/graph/CompiledPrograms.cpp
    src/graph/PlanDiagnostics.cpp
    src/graph/GraphCompiler.cpp
    src/graph/RenderingPlan.cpp
    src/graph/ConditionalPlan.cpp
    src/graph/SubmissionPlan.cpp
    src/graph/PhysicalResourcePlan.cpp
    src/graph/BarrierPlan.cpp
    src/graph/ShaderBindingPlan.cpp
    src/graph/Shader.cpp
    src/graph/RasterProgram.cpp
    src/graph/ShaderInterface.cpp
    src/upload/Preparation.cpp
    src/upload/ResourceUploader.cpp
    src/upload/StagingArena.cpp
    src/device/BufferArena.cpp
    src/resource/texture/SampledTexture2DStorage.cpp
    src/resource/material/RasterMaterialStorage.cpp
    src/resource/instance/RasterMeshInstanceStorage.cpp
    src/resource/mesh/IndexedTriangleMeshStorage.cpp
    src/runtime/RenderServer.cpp
    src/runtime/CapabilityRegistry.cpp
    src/runtime/CapabilityCompilation.cpp
    src/commands/CommandRegistry.cpp
    src/runtime/RenderServerImpl.cpp
    src/runtime/CapabilityCatalog.cpp
    src/runtime/ResourceScopes.cpp
    src/runtime/JobOutput.cpp
    src/target/SwapchainFlight.cpp
    src/runtime/FrameProgress.cpp
    src/runtime/JobControl.cpp
    src/runtime/JobRelations.cpp
    src/runtime/Scene.cpp
    src/runtime/EffectCompilation.cpp
    src/commands/CommandDispatchContext.cpp
    src/runtime/FrameSubmission.cpp
    src/commands/ResponseOutbox.cpp
)

set(mesh_shader_headers)
foreach(shader
    BasicMesh DepthOnly SurfaceMesh TangentMesh
    BasicMeshPaged DepthOnlyPaged SurfaceMeshPaged TangentMeshPaged
    BasicMeshNoUv SurfaceMeshNoUv TangentMeshNoUv
    BasicMeshNoUvPaged SurfaceMeshNoUvPaged TangentMeshNoUvPaged
)
    foreach(shadow 0 1)
        foreach(reflected 0 1)
            set(defines)
            if(shadow)
                list(APPEND defines -DSHADOW_PASS=1)
            endif()
            if(reflected)
                list(APPEND defines -DREFLECTED_INSTANCES=1)
            endif()
            if(shader MATCHES "^DepthOnly")
                list(APPEND defines -DDEPTH_ONLY=1)
            endif()
            if(shader MATCHES "^(SurfaceMesh|TangentMesh)")
                list(APPEND defines -DVERTEX_NORMALS=1)
            endif()
            if(shader MATCHES "^TangentMesh")
                list(APPEND defines -DVERTEX_TANGENTS=1)
            endif()
            if(shader MATCHES "NoUv")
                list(APPEND defines -DVERTEX_NO_UV=1)
            endif()
            if(shader MATCHES "Paged$")
                list(APPEND defines -DPAGED_INSTANCES=1)
            endif()
            set(variant "${shader}")
            if(shadow)
                string(APPEND variant "Shadow")
            endif()
            if(reflected)
                string(APPEND variant "Reflected")
            endif()
            set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${variant}Vertex.hpp")
            set(target_options)
            if(shader MATCHES "Paged$")
                list(APPEND target_options --physical-pointers)
            endif()
            lux_render_shader("${header}" shaders/mesh/IndexedTriangleMeshVertex.slang vertex
                k${variant}Vertex ${defines} ${target_options}
            )
            list(APPEND mesh_shader_headers "${header}")
        endforeach()
    endforeach()
endforeach()

set(hzb_header "${CMAKE_CURRENT_BINARY_DIR}/shaders/HzbDownsample.hpp")
lux_render_shader("${hzb_header}" shaders/hzb/Downsample.slang compute kHzbDownsample)

set(material_shader_root "${CMAKE_CURRENT_SOURCE_DIR}/../material/shaders")
set(feature_shader_headers)
foreach(shader
    FullscreenVertex
    DeferredLighting DeferredLightingShadow
    DeferredLightingClustered DeferredLightingClusteredShadow
    TonemapLinear TonemapSrgb
    UiRasterVertex UiRasterFragment UiOutputLinear UiOutputSrgb
)
    set(defines)
    if(shader STREQUAL "FullscreenVertex")
        set(source shaders/deferred/FullscreenVertex.slang)
    elseif(shader STREQUAL "UiRasterVertex")
        set(source shaders/ui/UiRasterVertex.slang)
    elseif(shader STREQUAL "UiRasterFragment")
        set(source shaders/ui/UiRasterFragment.slang)
    elseif(shader MATCHES "^UiOutput")
        set(source shaders/ui/UiOutputFragment.slang)
        if(shader STREQUAL "UiOutputSrgb")
            list(APPEND defines -DOUTPUT_SRGB=1)
        endif()
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
    )
endif()

target_compile_definitions(render PRIVATE LUX_RENDER_LIBRARY)
target_compile_features(render PUBLIC cxx_std_20)
target_link_libraries(render
    PUBLIC lux::cxx::container lux::cxx::compile_time lux::cxx::algorithm lux::cxx::core
        lux::engine::core::error_values lux::engine::resource::description_types
        lux::engine::resource::shader_metadata lux::engine::platform::platform_ipc
        STDEXEC::stdexec lux::cxx::execution lux::engine::core::core_execution
    PRIVATE NRI imgui::core Eigen3::Eigen lux::engine::core::math
        lux::engine::core::serialization
        spirv-cross-c SPIRV-Tools-static
        lux::engine::platform::platform_process
)

component_add_transitive_commands(render
    "find_package(stdexec CONFIG REQUIRED GLOBAL)"
    "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core execution)"
    "find_package(lux-engine-core CONFIG REQUIRED COMPONENTS error_values)"
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

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
