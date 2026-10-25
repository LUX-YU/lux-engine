find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core)

find_package(stdexec CONFIG REQUIRED GLOBAL)

find_package(Eigen3 CONFIG REQUIRED)

if(NOT TARGET lux::engine::core::serialization)
    find_package(lux-engine CONFIG REQUIRED COMPONENTS core::serialization)
endif()

find_package(Vulkan 1.3 REQUIRED)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../core/error/cmake/Values.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../resource/description/cmake/ValueTypes.cmake")
include(cmake/ShaderTools.cmake)
include(cmake/Nri.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../engine/toolchain/shader/cmake/MetadataCompiler.cmake")

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_RENDER_LIBRARY
    PUBLIC_MACRO_NAME LUX_RENDER_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_RENDER_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/render/visibility.h
)

set(render_sources
    src/core/Target.cpp
    src/core/Error.cpp
    src/core/RasterViewport.cpp
    src/commands/Command.cpp
    src/resource/StorageDescriptor.cpp
    src/resource/StorageFactory.cpp
    src/resource/ResourceRegistry.cpp
    src/resource/StorageSet.cpp
    src/storage/IndexedTriangleMesh.cpp
    src/storage/RasterMaterialTable.cpp
    src/graph/RenderGraphBuilder.cpp
    src/graph/RenderGraphAnalysis.cpp
    src/graph/Diagnostics.cpp
    src/features/FeatureAssembly.cpp
    src/features/FeatureBuildContext.cpp
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
    src/features/RasterAssets.cpp
    src/features/LitScene.cpp
    src/features/UiScene.cpp
    src/features/FullscreenRaster.cpp
    src/features/ParameterLayout.cpp
    src/storage/DirectLightingStorage.cpp
    src/storage/DirectLightingCommandHandler.cpp
    src/features/RasterMeshDraws.cpp
    src/device/Error.cpp
    src/device/DeviceSession.cpp
    src/device/DescriptorTable.cpp
    src/device/vulkan/DeviceBackend.cpp
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
    src/graph/PlanDiagnostics.cpp
    src/graph/GraphCompiler.cpp
    src/graph/RenderingPlan.cpp
    src/graph/ConditionalPlan.cpp
    src/graph/SubmissionPlan.cpp
    src/graph/PhysicalResourcePlan.cpp
    src/graph/BarrierPlan.cpp
    src/graph/PipelineLayoutPlan.cpp
    src/graph/Shader.cpp
    src/graph/ShaderInterface.cpp
    src/upload/ResourceUploader.cpp
    src/memory/StagingArena.cpp
    src/memory/BufferArena.cpp
    src/storage/SampledTexture2DStorage.cpp
    src/storage/SampledTexture2DCommandHandler.cpp
    src/storage/RasterMaterialTableStorage.cpp
    src/storage/RasterMaterialTableCommandHandler.cpp
    src/storage/RasterMeshInstanceStorage.cpp
    src/storage/RasterMeshInstanceCommandHandler.cpp
    src/storage/IndexedTriangleMeshStorage.cpp
    src/storage/IndexedTriangleMeshCommandHandler.cpp
    src/runtime/RenderServer.cpp
    src/runtime/CommandRegistry.cpp
    src/runtime/RenderServerImpl.cpp
    src/runtime/CapabilityInstallation.cpp
    src/runtime/Scene.cpp
    src/runtime/EffectCompilation.cpp
    src/runtime/CommandDispatchContext.cpp
    src/runtime/FrameSubmission.cpp
    src/runtime/ResponseOutbox.cpp
)

set(mesh_shader_headers)
foreach(shader
    BasicMesh DepthOnly SurfaceMesh TangentMesh
    BasicMeshPaged DepthOnlyPaged SurfaceMeshPaged TangentMeshPaged
    BasicMeshNoUv SurfaceMeshNoUv TangentMeshNoUv
    BasicMeshNoUvPaged SurfaceMeshNoUvPaged TangentMeshNoUvPaged
)
    foreach(reflected 0 1)
        set(defines)
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
        if(reflected)
            string(APPEND variant "Reflected")
        endif()
        set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${variant}Vertex.hpp")
        set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/${variant}Vertex.spv")
        add_custom_command(
            OUTPUT "${header}"
            BYPRODUCTS "${binary}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
            COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S vert
                ${defines} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh/IndexedTriangleMesh.vert" -o "${binary}"
            COMMAND lux_shader_metadata "${binary}" "${header}" k${variant}Vertex
            DEPENDS shaders/mesh/IndexedTriangleMesh.vert lux_shader_metadata
            VERBATIM
        )
        list(APPEND mesh_shader_headers "${header}")
    endforeach()
endforeach()

set(hzb_header "${CMAKE_CURRENT_BINARY_DIR}/shaders/HzbDownsample.hpp")
set(hzb_binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/HzbDownsample.spv")
add_custom_command(
    OUTPUT "${hzb_header}"
    BYPRODUCTS "${hzb_binary}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hzb/Downsample.comp" -o "${hzb_binary}"
    COMMAND lux_shader_metadata "${hzb_binary}" "${hzb_header}" kHzbDownsample
    DEPENDS shaders/hzb/Downsample.comp lux_shader_metadata
    VERBATIM
)

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
        set(source shaders/deferred/Fullscreen.vert)
    elseif(shader STREQUAL "UiRasterVertex")
        set(source shaders/ui/UiRaster.vert)
    elseif(shader STREQUAL "UiRasterFragment")
        set(source shaders/ui/UiRaster.frag)
    elseif(shader MATCHES "^UiOutput")
        set(source shaders/ui/UiOutput.frag)
        if(shader STREQUAL "UiOutputSrgb")
            list(APPEND defines -DOUTPUT_SRGB=1)
        endif()
    elseif(shader MATCHES "^DeferredLighting")
        set(source shaders/deferred/DeferredLighting.frag)
        if(shader MATCHES "Shadow$")
            list(APPEND defines -DSHADOWS=1)
        endif()
        if(shader MATCHES "Clustered")
            list(APPEND defines -DCLUSTERED=1)
        endif()
    else()
        set(source shaders/postprocess/Tonemap.frag)
        if(shader STREQUAL "TonemapSrgb")
            list(APPEND defines -DOUTPUT_SRGB=1)
        endif()
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.hpp")
    set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv")
    add_custom_command(
        OUTPUT "${header}"
        BYPRODUCTS "${binary}"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3
            "-I${material_shader_root}" ${defines} "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${binary}"
        COMMAND lux_shader_metadata "${binary}" "${header}" k${shader}
        DEPENDS "${source}" lux_shader_metadata
            shaders/shadow/ShadowPcf.glsl
            shaders/deferred/ClusterLookup.glsl
            "${material_shader_root}/gbuffer_encode.glsl"
            "${material_shader_root}/brdf/brdf_common.glsl"
            "${material_shader_root}/brdf/brdf_ggx.glsl"
            "${material_shader_root}/brdf/direct_lighting.glsl"
        VERBATIM
    )
    list(APPEND feature_shader_headers "${header}")
endforeach()

foreach(shader MeshFrustumCull MeshFrustumCullPaged MeshCompact MeshView)
    set(defines)
    if(shader STREQUAL "MeshFrustumCullPaged")
        set(source shaders/mesh/MeshFrustumCull.comp)
        list(APPEND defines -DPAGED_INSTANCES=1)
    else()
        set(source "shaders/mesh/${shader}.comp")
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.hpp")
    set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv")
    add_custom_command(
        OUTPUT "${header}"
        BYPRODUCTS "${binary}"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3
            ${defines} "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${binary}"
        COMMAND lux_shader_metadata "${binary}" "${header}" k${shader}
        DEPENDS "${source}" lux_shader_metadata
        VERBATIM
    )
    list(APPEND feature_shader_headers "${header}")
endforeach()

foreach(stage build count scan fill)
    string(SUBSTRING "${stage}" 0 1 first)
    string(TOUPPER "${first}" first)
    string(SUBSTRING "${stage}" 1 -1 rest)
    set(shader "Cluster${first}${rest}")
    set(source "shaders/deferred/cluster_${stage}.comp")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.hpp")
    set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv")
    add_custom_command(
        OUTPUT "${header}"
        BYPRODUCTS "${binary}"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3
            "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${binary}"
        COMMAND lux_shader_metadata "${binary}" "${header}" k${shader}
        DEPENDS "${source}" shaders/deferred/ClusterIntersection.glsl lux_shader_metadata
        VERBATIM
    )
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
target_link_libraries(render_extensions INTERFACE render STDEXEC::stdexec lux::engine::core::core_execution)

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

target_compile_definitions(render PRIVATE LUX_RENDER_LIBRARY)
target_compile_features(render PUBLIC cxx_std_20)
target_link_libraries(render
    PUBLIC lux::cxx::container lux::cxx::compile_time lux::cxx::algorithm lux::cxx::core
        lux::engine::core::error_values lux::engine::resource::description_types
        lux::engine::resource::shader_metadata lux::engine::platform::platform_ipc
    PRIVATE NRI Eigen3::Eigen STDEXEC::stdexec lux::engine::core::core_execution
        lux::engine::core::serialization
        lux_render_spirv_cross_c_shared lux_render_SPIRV_Tools_shared
        lux::engine::platform::platform_process
)

if(WIN32)
    # Build-tree consumers use the same real compiler DLLs as the install; no SDK PATH fallback.
    add_custom_command(
        TARGET render POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:lux_render_spirv_cross_c_shared>"
            "$<TARGET_FILE:lux_render_SPIRV_Tools_shared>"
            "$<TARGET_FILE_DIR:render>"
        VERBATIM
    )
endif()

component_add_transitive_commands(render
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core)"
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
