find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core)
find_package(stdexec CONFIG REQUIRED)
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
    src/features/RasterMeshDraws.cpp
    src/device/Error.cpp
    src/device/DeviceSession.cpp
    src/device/DescriptorTable.cpp
    src/device/vulkan/DeviceBackend.cpp
    src/device/SubmissionQueue.cpp
    src/execution/FrameExecution.cpp
    src/execution/FrameResources.cpp
    src/execution/OffscreenTarget.cpp
    src/execution/RenderGraphInstance.cpp
    src/execution/RenderExecutor.cpp
    src/graph/RenderPlan.cpp
    src/graph/PlanDiagnostics.cpp
    src/graph/GraphCompiler.cpp
    src/graph/RenderingPlan.cpp
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
    src/runtime/RegistrationBatch.cpp
    src/runtime/RenderServerImpl.cpp
    src/runtime/EffectCompilation.cpp
    src/runtime/ResponseDelivery.cpp
    src/runtime/CommandDispatchContext.cpp
)

set(mesh_shader_headers)
foreach(shader BasicMesh DepthOnly BasicMeshPaged DepthOnlyPaged)
    set(defines)
    if(shader MATCHES "^DepthOnly")
        list(APPEND defines -DDEPTH_ONLY=1)
    endif()
    if(shader MATCHES "Paged$")
        list(APPEND defines -DPAGED_INSTANCES=1)
    endif()
    set(header "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}Vertex.hpp")
    set(binary "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}Vertex.spv")
    add_custom_command(
        OUTPUT "${header}"
        BYPRODUCTS "${binary}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}" -V --target-env vulkan1.3 -S vert
            ${defines} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh/IndexedTriangleMesh.vert" -o "${binary}"
        COMMAND lux_shader_metadata "${binary}" "${header}" k${shader}Vertex
        DEPENDS shaders/mesh/IndexedTriangleMesh.vert lux_shader_metadata
        VERBATIM
    )
    list(APPEND mesh_shader_headers "${header}")
endforeach()

add_component(
    COMPONENT_NAME render
    NAMESPACE lux::engine::function
    OUTPUT_NAME lux_engine_render
    SOURCE_FILES ${render_sources} ${mesh_shader_headers}
)

component_include_directories(
    render
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include" "${LUX_GENERATE_HEADER_DIR}"
    INSTALL_TIME include
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/sinclude" "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
        "${CMAKE_CURRENT_BINARY_DIR}/shaders" "${LUX_NRI_VMA_HEADERS}" "${LUX_NRI_VULKAN_HEADERS}"
)

target_compile_definitions(render PRIVATE LUX_RENDER_LIBRARY)
target_compile_features(render PUBLIC cxx_std_20)
target_link_libraries(render
    PUBLIC lux::cxx::container lux::cxx::compile_time lux::cxx::algorithm lux::cxx::core
        lux::engine::core::error_values lux::engine::resource::description_types
        lux::engine::resource::shader_metadata STDEXEC::stdexec
        lux::engine::core::core_execution lux::engine::platform::platform_ipc
    PRIVATE NRI
        lux_render_spirv_cross_c_shared lux_render_SPIRV_Tools_shared
        lux::engine::platform::platform_process
)

component_add_transitive_commands(render
    "find_package(lux-cxx CONFIG REQUIRED COMPONENTS container compile_time algorithm core)"
    "find_package(stdexec CONFIG REQUIRED)"
    "find_package(lux-engine-core CONFIG REQUIRED COMPONENTS error_values)"
    "find_package(lux-engine-resource CONFIG REQUIRED COMPONENTS description_types shader_metadata)"
    "find_package(lux-engine-core-execution CONFIG REQUIRED COMPONENTS core_execution)"
    "find_package(lux-engine-platform CONFIG REQUIRED COMPONENTS platform_ipc)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-render
    VERSION 0.0.1
    NAMESPACE lux::engine::function
    COMPONENTS render
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
