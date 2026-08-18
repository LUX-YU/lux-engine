find_package(Vulkan REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS core container concurrent memory)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

set(RENDER_VULKAN_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/comm/FeatureRegistration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/comm/RenderServer.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/comm/RenderServerBootstrap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/comm/RenderServerFacade.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/comm/RenderServerHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/FrustumCuller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/descriptor/DescriptorService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/descriptor/SceneDescriptorArena.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/descriptor/SceneDomainDescriptorSets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/lifecycle/DeferredDestroyQueue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/lifecycle/VRAMBudgetGuard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/memory/ArenaAllocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/memory/GPUBufferVma.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/memory/StagingBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/memory/VmaTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/pipeline/GeneralDescriptorSetLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/pipeline/PipelineLayoutService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/pipeline/PipelineManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/pipeline/SpirvPatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/pipeline/StandardPipelineLayoutBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/RenderContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/RenderSurface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/transfer/TransferScheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/utils/StagingRingBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/VmaImplementation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/VulkanContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/FrameExtensionRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/FrameExternalSync.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/KernelRegistry.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RenderGraphCompiler.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RenderGraphCompiler.ExecutionPlan.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RenderGraphCompiler.Barriers.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RenderGraphCompiler.LayoutAllocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RenderPassPlanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RGBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RGDebugPrint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RGGraphResize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RGVulkanRecorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RGVulkanRecorder.RecordContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RGVulkanRecorder.Submit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/graph/RGVulkanResourceAllocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderContextView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/FrameDriver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/FrameOrchestrator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/Renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/RenderTargetRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/descriptor/BindlessCombinedSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/hzb/HzbResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/lifecycle/GpuTransferPipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/lighting/LightResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/lighting/ShadowResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/material/MaterialResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/material/TextureSamplingRepresentationCatalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/material/VariantBucketManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/mesh/InstanceResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/mesh/InstanceSlotRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/mesh/SparseInstanceStream.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/mesh/MdcTable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/mesh/MeshResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/mesh/MeshSectionTable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/point_cloud/GpuOctreeNodeBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/point_cloud/PointCloudGlobalBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/ShaderResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/TextureResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/TrajectoryGlobalBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/vertex/IVertexSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/vertex/SkinningResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/vertex/StaticVertexSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/vertex/TransientVertexSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/vertex/VertexPoolRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/resources/vertex/VertexProduction.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/RenderScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/RenderSceneView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/SceneFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/SceneFeatureSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/SceneGraphCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/SceneViewSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/View.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/targets/OffscreenImagePool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/targets/PresentContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/targets/RenderTargetBinding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/targets/SwapchainProvider.cpp
)

add_component(
    COMPONENT_NAME render_vulkan
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${RENDER_VULKAN_SRCS}
)

component_include_directories(
    render_vulkan
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        ${CMAKE_BINARY_DIR}/gen
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME
        include
)

target_compile_definitions(
    render_vulkan
    PRIVATE
        LUX_FUNCTION_LIBRARY
)

if(WIN32)
    target_compile_definitions(
        render_vulkan
        PRIVATE
            VK_USE_PLATFORM_WIN32_KHR
            NOMINMAX
    )
endif()

component_add_transitive_commands(
    render_vulkan
    "find_package(lux-engine-platform REQUIRED COMPONENTS window gapi common)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
    "find_package(lux-cxx REQUIRED COMPONENTS core container concurrent memory)"
)

component_add_internal_dependencies(render_vulkan render_client render_graph)

lux_classify_target(
    TARGET  render_vulkan
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

target_link_libraries(
    render_vulkan
    PUBLIC
        lux::cxx::core
        lux::cxx::container
        lux::cxx::concurrent
        lux::engine::platform::window
        lux::engine::platform::gapi
        lux::engine::platform::common
        lux::cxx::memory
        lux::engine::core::math
        lux::engine::resource::description
    PRIVATE
        GPUOpen::VulkanMemoryAllocator
)

# libandroid provides ANativeWindow_getWidth/Height for RenderSurface.
if(ANDROID)
    target_link_libraries(render_vulkan PRIVATE android)
endif()
