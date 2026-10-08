find_package(Vulkan REQUIRED)

option(LUX_RENDER_BUILD_GPU_TESTS "Register native-device Vulkan ownership tests" OFF)

option(LUX_RENDER_LIFECYCLE_DIAGNOSTICS "Private lifecycle rejection stimulus; never enable in a normal SDK" OFF)
if(LUX_RENDER_LIFECYCLE_DIAGNOSTICS AND (NOT BUILD_TESTING OR NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"))
    message(FATAL_ERROR "Render lifecycle diagnostics require BUILD_TESTING and RelWithDebInfo")
endif()
find_package(lux-cxx REQUIRED COMPONENTS core container concurrent memory)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)

set(RENDER_VULKAN_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/comm/FeatureParamsHandlers.cpp
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/RenderFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/RenderFeatureSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/SceneGraphCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/SceneViewSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/View.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/targets/OffscreenImagePool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/targets/PresentContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/targets/RenderTargetBinding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/targets/ClearRenderTarget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/targets/SwapchainProvider.cpp
)

add_component(
    COMPONENT_NAME render_vulkan
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${RENDER_VULKAN_SRCS}
)
if(LUX_RENDER_LIFECYCLE_DIAGNOSTICS)
    target_compile_definitions(render_vulkan PRIVATE LUX_RENDER_LIFECYCLE_DIAGNOSTICS)
endif()

component_include_directories(
    render_vulkan
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_BINARY_DIR}/gen
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
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
    "find_package(lux-engine-platform REQUIRED COMPONENTS window gapi)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
    "find_package(lux-cxx REQUIRED COMPONENTS core container concurrent memory)"
)

component_add_internal_dependencies(render_vulkan render_feature_client render_graph)

target_link_libraries(
    render_vulkan
    PUBLIC
        lux::cxx::core
        lux::cxx::container
        lux::cxx::concurrent
        lux::engine::platform::window
        lux::engine::platform::gapi
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

if(BUILD_TESTING)
    add_executable(render_arena_lifecycle_test test/arena_lifecycle.cpp)
    target_include_directories(render_arena_lifecycle_test PRIVATE sinclude)
    target_compile_definitions(render_arena_lifecycle_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(
        render_arena_lifecycle_test
        PRIVATE render_client GPUOpen::VulkanMemoryAllocator Vulkan::Vulkan
    )
    if(MSVC)
        target_compile_options(render_arena_lifecycle_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_arena_lifecycle_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME render.arena_lifecycle COMMAND render_arena_lifecycle_test)
    set_tests_properties(render.arena_lifecycle PROPERTIES TIMEOUT 30)

    add_executable(render_device_objects_test test/device_objects.cpp)
    target_include_directories(render_device_objects_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_device_objects_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_device_objects_test PRIVATE render_client Vulkan::Vulkan)
    if(MSVC)
        target_compile_options(render_device_objects_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_device_objects_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME render.device_objects COMMAND render_device_objects_test)
    set_tests_properties(render.device_objects PROPERTIES TIMEOUT 30)

    add_executable(
        render_shader_ownership_test
        test/shader_ownership.cpp
        src/gpu/pipeline/SpirvPatcher.cpp
    )
    target_include_directories(render_shader_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_shader_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_shader_ownership_test PRIVATE render_vulkan)
    if(MSVC)
        target_compile_options(render_shader_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_shader_ownership_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME render.shader_ownership COMMAND render_shader_ownership_test)
    set_tests_properties(render.shader_ownership PROPERTIES TIMEOUT 30)

    add_executable(render_general_layout_ownership_test test/general_layout_ownership.cpp)
    target_include_directories(render_general_layout_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_general_layout_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_general_layout_ownership_test PRIVATE render_vulkan)
    if(MSVC)
        target_compile_options(render_general_layout_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_general_layout_ownership_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.general_layout_ownership COMMAND render_general_layout_ownership_test)
        set_tests_properties(render.general_layout_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()

    add_executable(render_transfer_ownership_test test/transfer_ownership.cpp)
    target_include_directories(render_transfer_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_transfer_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_transfer_ownership_test PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator)
    if(MSVC)
        target_compile_options(render_transfer_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_transfer_ownership_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.transfer_ownership COMMAND render_transfer_ownership_test)
        set_tests_properties(render.transfer_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()

    add_executable(render_vma_allocator_ownership_test test/vma_allocator_ownership.cpp)
    target_include_directories(render_vma_allocator_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_vma_allocator_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_vma_allocator_ownership_test PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator)
    if(MSVC)
        target_compile_options(render_vma_allocator_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_vma_allocator_ownership_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.vma_allocator_ownership COMMAND render_vma_allocator_ownership_test)
        set_tests_properties(render.vma_allocator_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()

    add_executable(render_offscreen_ownership_test test/offscreen_ownership.cpp)
    target_include_directories(render_offscreen_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_offscreen_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_offscreen_ownership_test PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator)
    if(MSVC)
        target_compile_options(render_offscreen_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_offscreen_ownership_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.offscreen_ownership COMMAND render_offscreen_ownership_test)
        set_tests_properties(render.offscreen_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()

    add_executable(render_record_context_ownership_test test/record_context_ownership.cpp)
    target_include_directories(render_record_context_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_record_context_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_record_context_ownership_test PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator)
    if(MSVC)
        target_compile_options(render_record_context_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_record_context_ownership_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.record_context_ownership COMMAND render_record_context_ownership_test)
        set_tests_properties(render.record_context_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()

    add_executable(render_one_time_ownership_test test/one_time_ownership.cpp)
    target_include_directories(render_one_time_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_one_time_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_one_time_ownership_test PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator)
    if(MSVC)
        target_compile_options(render_one_time_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_one_time_ownership_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.one_time_ownership COMMAND render_one_time_ownership_test)
        set_tests_properties(render.one_time_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()

    add_executable(render_vma_pool_ownership_test test/vma_pool_ownership.cpp)
    target_include_directories(render_vma_pool_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_vma_pool_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_vma_pool_ownership_test PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator)
    if(MSVC)
        target_compile_options(render_vma_pool_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_vma_pool_ownership_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.vma_pool_ownership COMMAND render_vma_pool_ownership_test)
        set_tests_properties(render.vma_pool_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()

    add_executable(render_pipeline_ownership_test test/pipeline_ownership.cpp)
    target_include_directories(render_pipeline_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_pipeline_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(render_pipeline_ownership_test PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator)
    if(MSVC)
        target_compile_options(render_pipeline_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_pipeline_ownership_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.pipeline_ownership COMMAND render_pipeline_ownership_test)
        set_tests_properties(render.pipeline_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()
    add_executable(render_vma_allocation_ownership_test test/vma_allocation_ownership.cpp)
    target_include_directories(render_vma_allocation_ownership_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_vma_allocation_ownership_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(
        render_vma_allocation_ownership_test
        PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator
    )
    if(MSVC)
        target_compile_options(render_vma_allocation_ownership_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_vma_allocation_ownership_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.vma_allocation_ownership COMMAND render_vma_allocation_ownership_test)
        set_tests_properties(render.vma_allocation_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()

    add_executable(
        render_resource_context_test
        test/resource_context_construction.cpp
        test/resource_context_server.cpp
    )
    target_include_directories(render_resource_context_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_resource_context_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(
        render_resource_context_test
        PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator
    )
    if(MSVC)
        target_compile_options(render_resource_context_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_resource_context_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.resource_context_construction COMMAND render_resource_context_test)
        set_tests_properties(render.resource_context_construction PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()

    add_executable(render_mesh_construction_test test/mesh_construction.cpp)
    target_include_directories(render_mesh_construction_test PRIVATE sinclude pinclude)
    target_compile_definitions(render_mesh_construction_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
    target_link_libraries(
        render_mesh_construction_test
        PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator
    )
    if(MSVC)
        target_compile_options(render_mesh_construction_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(render_mesh_construction_test PRIVATE -UNDEBUG)
    endif()
    if(LUX_RENDER_BUILD_GPU_TESTS)
        add_test(NAME render.mesh_geometry_ownership COMMAND render_mesh_construction_test)
        set_tests_properties(render.mesh_geometry_ownership PROPERTIES TIMEOUT 30 LABELS gpu)
    endif()
endif()
