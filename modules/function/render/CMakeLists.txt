find_package(Vulkan  REQUIRED COMPONENTS glslc)
find_package(lux-cxx REQUIRED COMPONENTS reflection_runtime container concurrent)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_path(meshoptimizer_INCLUDE_DIRS NAMES meshoptimizer.h REQUIRED)
message("---- GLSL Compiler path: ${Vulkan_GLSLC_EXECUTABLE}")

set(SHADER_SOURCE_PATH      ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders)
set(GLSL_SHADER_SOURCE_PATH ${SHADER_SOURCE_PATH}/glsl)
set(ASSET_GENERATE_PATH     ${CMAKE_BINARY_DIR}/assets)
set(TEXTURE_SOURCE_PATH     ${CMAKE_CURRENT_SOURCE_DIR}/assets/textures)

include_component_cmake_scripts(lux::cxx::reflection_runtime)
include_component_cmake_scripts(meta)
include_component_cmake_scripts(lux::engine::resource::asset)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/compile_glsl_shaders.cmake)

set(RENDER_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/scene/RenderScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/scene/View.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/scene/SpatialCullGrid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/comm/FeatureRegistration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/FrameExternalSync.cpp
    # Comm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/comm/RenderServer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/comm/RenderServerHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/comm/RenderSession.cpp
    # Core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/core/RenderTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/core/VulkanContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/core/VmaImplementation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/core/Errors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/utils/StagingRingBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/core/RenderSurface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/targets/SwapchainProvider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/targets/OffscreenImagePool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/FrameDriver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/material/MaterialResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/material/VariantBucketManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/mesh/MeshResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/ShaderResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/TextureResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/lighting/LightResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/descriptor/DescriptorService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/descriptor/SceneDescriptorArena.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/lifecycle/DeferredDestroyQueue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/memory/StagingBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/memory/GPUBufferVma.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/lifecycle/VRAMBudgetGuard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/memory/ArenaAllocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/memory/VmaTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/mesh/MdcTable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/pipeline/GeneralDescriptorSetLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/pipeline/PipelineLayoutService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/pipeline/StandardPipelineLayoutBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/pipeline/PipelineManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RGDepAnalyzer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RenderPassPlanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RenderGraphCompiler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/KernelRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/FrameExtensionRegistry.cpp
    # Kernel descriptor registrations
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/kernels/MeshKernels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/kernels/ShadowKernels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/kernels/UtilityKernels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RGVulkanResourceAllocator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RGVulkanRecorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RGVulkanRecorder.RecordContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RGVulkanRecorder.Submit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RGGraphResize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RGBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/graph/RGDebugPrint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/descriptor/BindlessCombinedSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/RenderContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/RenderContextView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/RenderSceneView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/FrameOrchestrator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/RenderFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/Renderer.cpp
    # Features — skinning (R5 of render-refactor)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/skinning/SkinningFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/skinning/SkinningOperationHandlers.cpp
    # HZB occlusion pyramid (P2 Stage A)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/hzb/HzbResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/hzb/HzbFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/hzb/HzbOperationHandlers.cpp
    # Features — grid
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/grid/GridPassFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/grid/GridOperationHandlers.cpp
    # Features — sky_box
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/sky_box/SkyboxFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/sky_box/SkyboxOperationHandlers.cpp
    # Features — deferred
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/deffer/DeferredGBufferFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/deffer/DeferredGBufferOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/deffer/DeferredLightingFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/deffer/DeferredLightingOperationHandlers.cpp
    # Features — postprocess
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/postprocess/TonemapFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/postprocess/TonemapOperationHandlers.cpp
    # Features — forward
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/GpuDrivenMeshFeatureBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/forward/ForwardMeshFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/forward/ForwardMeshOperationHandlers.cpp
    # Features — point_cloud
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/point_cloud/PointCloudOctree.cpp
    # Features — point_cloud (queue-based architecture)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/point_cloud/PCFeatureSimple.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/point_cloud/PCFeatureIndirectBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/point_cloud/PCFeatureGPUDriven.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/point_cloud/PCFeatureLOD.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/point_cloud/PCFeatureSplatting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/point_cloud/PCFeatureTransient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/point_cloud/PCOperationHandlers.cpp
    # Features — particle (excluded from minimum set)
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/particle/ParticleFeature.cpp
    # Features — depth prepass (no subfolder yet)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/DepthPrepassFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/DepthPrepassOperationHandlers.cpp
    # Features — shadow
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/shadow/ShadowMapFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/shadow/ShadowMapOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/shadow/MeshShadowFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/shadow/MeshShadowOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/shadow/EVSMShadowResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/shadow/EVSMShadowTechnique.cpp
    # Renderer misc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/FrustumCuller.cpp
    # Pipeline
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/pipeline/MaterialPipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/pipeline/ShadingModelRegistry.cpp
    # Resources
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/lifecycle/UploadWorkerPool.cpp
    # ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/ParticleResources.cpp  # excluded: particle
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/lighting/ShadowResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/mesh/InstanceResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/mesh/InstanceSlotRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/vertex/IVertexSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/vertex/StaticVertexSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/vertex/TransientVertexSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/vertex/VertexPoolRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/vertex/VertexProduction.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/vertex/SkinningResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/mesh/MeshSectionTable.cpp
    # Resources — point cloud
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/point_cloud/PointCloudGlobalBuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/point_cloud/GpuOctreeNodeBuffer.cpp
    # Features — trajectory
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/trajectory/TrajectoryLineFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/trajectory/TrajectoryOperationHandlers.cpp
    # Features — gizmo
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/gizmo/LineListTransientFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/gizmo/LineListOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/gizmo/TriOverlayTransientFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/gizmo/TriOverlayOperationHandlers.cpp
    # Features — highlight outline
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/highlight/HighlightFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/highlight/HighlightOperationHandlers.cpp
    # Features — spatial coarse cull (cell-grid distance cull; owns SpatialCullGrid)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/spatialcull/SpatialCullFeature.cpp
    # Features — standard 3D mesh stack (owns per-scene InstanceResources / VertexPool / MeshInputPool + instance ops)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/meshstack/StandardMeshStackFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/meshstack/MeshStackOperationHandlers.cpp
    # Features — standard material stack (owns global ShadingModelRegistry + MaterialResources + material ops)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/material/StandardMaterialFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/material/MaterialOperationHandlers.cpp
    # Features — standard view camera (owns per-scene ViewCameraResource + per-view camera update op)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/view_camera/StandardViewCameraFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/view_camera/ViewCameraOperationHandlers.cpp
    # Features — light (owns per-scene LightResources + light CRUD ops)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/light/LightFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/light/LightOperationHandlers.cpp
    # Features — Canvas2D (SinglePerScene 2D draw-batch owner; sprite/tilemap/pixel bridges → SceneColor)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/canvas2d/Canvas2DFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/canvas2d/Canvas2DOperationHandlers.cpp
    # Features — generic per-feature setParams op (reflection-driven settings panel)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/renderer/features/FeatureParamsHandlers.cpp
    # Resources — trajectory
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/resources/TrajectoryGlobalBuffer.cpp
    # Transfer subsystem
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/transfer/TransferScheduler.cpp
)

add_component(
    COMPONENT_NAME render
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${RENDER_SRCS}
)

target_include_directories(
    render
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)

component_include_directories(
    render
    BUILD_TIME_EXPORT  
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        ${CMAKE_BINARY_DIR}/gen
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${meshoptimizer_INCLUDE_DIRS}
    INSTALL_TIME
        include
)

component_add_cmake_scripts(
    render
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/compile_glsl_shaders.cmake
)

target_compile_definitions(
    render
    PRIVATE
    LUX_FUNCTION_LIBRARY
)

get_target_property(INC_DIRS render INCLUDE_DIRECTORIES)

if(WIN32)
target_compile_definitions(
	render
	PRIVATE
	VK_USE_PLATFORM_WIN32_KHR
	NOMINMAX
)
endif()

component_add_transitive_commands(
    render
    "find_package (lux-engine-platform REQUIRED COMPONENTS window gapi common)"
    "find_package (lux-engine-core     REQUIRED COMPONENTS meta math)"
    "find_package (lux-engine-resource REQUIRED COMPONENTS asset description)"
    "find_package(lux-cxx              REQUIRED COMPONENTS compile_time)"
)

# ---------------------------------------------------------------------------
# Shader pipeline: compile GLSL -> SPIR-V -> pack .luxasset
# RENDER_SHADER_SPVS receives intermediate .spv paths for build-time use only.
# ---------------------------------------------------------------------------
compile_and_pack_shaders(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render
	SHADER_DIR		${SHADER_SOURCE_PATH}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${SHADER_SOURCE_PATH}
	SHADERS
		# Grid
		grid/grid.vert                      grid/grid.frag
		# Depth prepass
		depth_prepass/depth_only.frag
		# Point cloud
		point_cloud/pointcloud_simple.vert  point_cloud/pointcloud_simple.frag
		point_cloud/pointcloud_lod.vert     point_cloud/pointcloud_splat.frag
		# Point cloud compute
		point_cloud/pointcloud_culling.comp
		# GPU-driven (unified). mesh_cull_unified.comp moved to multi-variant
		# (no_hzb / hzb) compile below — see compile_and_pack_shader_variants.
        forward/mdc_compact.comp
        forward/clear_count_buffers.comp
		# HZB occlusion pyramid build (P2 Stage A)
		hzb/downsample.comp
		# P3.2 R6: single mesh vertex shader path (the _vp variant; legacy
		# attribute-path forward_mesh.vert / mesh_shadow.vert deleted).
		forward/forward_mesh_vp.vert
		shadow/mesh_shadow_vp.vert
		# Depth-only _vp vert paired with shadow_depth.frag. Identical pool /
		# instance / visible path to mesh_shadow_vp.vert but drops the
		# EVSM-only varyings (vShadowNear / vShadowFar / vDepthPersp) — those
		# would trip the SPIR-V interface validator's OutputNotConsumed warning
		# when the fat EVSM vert is paired with the thin depth-only frag.
		# Do NOT use the legacy attribute-path shadow/shadow_depth.vert here;
		# it reads inPosition / inUV from vertex attributes and would render
		# blank shadows with the bindless pool pipeline (no attribute buffer
		# bound; see vertex_bindings.clear() in MeshShadowFeature.cpp).
		shadow/shadow_depth_vp.vert
		# Skinning (compute pre-skinning) — R5 of render-refactor
		skinning/skin_compute.comp
		# Skybox
		skybox/skybox.vert                  skybox/skybox_cubemap.frag     
        skybox/skybox_equirect.frag
		# Shading models (require INCLUDE_DIRS for brdf/ and lighting headers)
		# NOTE: fr_pbr.frag and fr_stylized.frag are compiled below as
		# multi-variant (PCF / EVSM) via compile_and_pack_shader_variants —
		# they include the shadow technique impl indirectly through
		# LUX_SHADOW_SAMPLER_HEADER.
        forward/fr_unlit.frag
		# Particle
		particle/particle.vert              particle/particle.frag         
        particle/particle_update.comp
		# Trajectory
		trajectory/trajectory_line.vert     trajectory/trajectory_line.frag
		# Shadow mapping
		shadow/shadow_depth.frag
		# EVSM technique (C3): moment caster + separable Gaussian blur.
		shadow/shadow_evsm_caster.frag
		shadow/shadow_evsm_blur_h.comp
		shadow/shadow_evsm_blur_v.comp
		# Deferred rendering — P3.2 R6: single _vp gbuffer vertex shader.
        deferred/gbuffer_vp.vert            deferred/gbuffer_unlit.frag
        deferred/gbuffer_pbr.frag
        deferred/gbuffer_stylized.frag
		# deferred_lighting.frag is compiled below via compile_and_pack_shader_variants
		# (PCF / EVSM variants of LUX_SHADOW_SAMPLER_HEADER).
		deferred/deferred_lighting.vert
		deferred/cluster_build.comp
		deferred/cluster_count.comp
		deferred/cluster_scan.comp
		deferred/cluster_fill.comp
		# Post-processing
		postprocess/tonemap.vert            postprocess/tonemap.frag
		# Gizmo
		gizmo/line_list.vert                gizmo/line_list.frag
		gizmo/tri_overlay.vert              gizmo/tri_overlay.frag
		# Canvas2D (2D sprite draw-batch)
		canvas2d/sprite.vert                canvas2d/sprite.frag
		# Highlight outline
		highlight/highlight_mask.vert       highlight/highlight_mask.frag
		highlight/highlight_blur.frag       highlight/highlight_composite.frag
)

# ---------------------------------------------------------------------------
# Shadow technique variants — single .frag source compiled with different
# LUX_SHADOW_SAMPLER_HEADER injections (PCF / EVSM / future MSM, VSM ...).
# See .internal/plan/evsm-shadow-implementation-guide.md.
# ---------------------------------------------------------------------------
set(_shadow_variants
	"pcf|LUX_SHADOW_TECHNIQUE_ID=0"
	"evsm|LUX_SHADOW_TECHNIQUE_ID=1"
)
compile_and_pack_shader_variants(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render
	SHADER_DIR		${SHADER_SOURCE_PATH}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${SHADER_SOURCE_PATH}
	SHADER			deferred/deferred_lighting.frag
	VARIANTS		${_shadow_variants}
)
compile_and_pack_shader_variants(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render
	SHADER_DIR		${SHADER_SOURCE_PATH}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${SHADER_SOURCE_PATH}
	SHADER			forward/fr_pbr.frag
	VARIANTS		${_shadow_variants}
)
compile_and_pack_shader_variants(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render
	SHADER_DIR		${SHADER_SOURCE_PATH}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${SHADER_SOURCE_PATH}
	SHADER			forward/fr_stylized.frag
	VARIANTS		${_shadow_variants}
)

# ---------------------------------------------------------------------------
# GPU-driven cull kernel — HZB on/off build-time variants. The no_hzb variant
# does NOT declare descriptor set 1 (HZB sampler + view UBO); the hzb variant
# does. A spec-constant fold cannot remove a statically-used resource from the
# SPIR-V, so shadow / HZB-off cull pipelines (set-0-only layout) must use the
# no_hzb variant — see VUID-VkComputePipelineCreateInfo-layout-07988.
# ---------------------------------------------------------------------------
set(_cull_hzb_variants
	"no_hzb|"
	"hzb|HZB_ENABLED=1"
)
compile_and_pack_shader_variants(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render
	SHADER_DIR		${SHADER_SOURCE_PATH}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${SHADER_SOURCE_PATH}
	SHADER			forward/mesh_cull_unified.comp
	VARIANTS		${_cull_hzb_variants}
)

# ---------------------------------------------------------------------------
# Embed built-in feature shaders as constexpr C++ byte arrays
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_builtin_shaders.cmake)
embed_builtin_shaders(
	TARGET		render
	ASSET_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_DIR	${CMAKE_CURRENT_BINARY_DIR}/builtin_shaders
	NAMESPACE	"lux::render::builtin"
	SHADERS
		# Point cloud (5)
		point_cloud/pointcloud_simple.vert
		point_cloud/pointcloud_simple.frag
		point_cloud/pointcloud_lod.vert
		point_cloud/pointcloud_splat.frag
		point_cloud/pointcloud_culling.comp
		# Grid (2)
		grid/grid.vert
		grid/grid.frag
		# Trajectory (2)
		trajectory/trajectory_line.vert
		trajectory/trajectory_line.frag
		# Deferred GBuffer — P3.2 R6: single _vp vertex shader.
		deferred/gbuffer_vp.vert
		deferred/gbuffer_unlit.frag
		deferred/gbuffer_pbr.frag
		deferred/gbuffer_stylized.frag
		# Deferred Lighting (vert + clustered; .frag is variant-baked below)
		deferred/deferred_lighting.vert
		deferred/cluster_build.comp
		deferred/cluster_count.comp
		deferred/cluster_scan.comp
		deferred/cluster_fill.comp
		# Deferred Lighting fragment — multi-variant via LUX_SHADOW_SAMPLER_HEADER.
		deferred/deferred_lighting.frag.pcf
		deferred/deferred_lighting.frag.evsm
		# Forward / shared compute — P3.2 R6: single _vp mesh vertex shader.
		forward/mesh_cull_unified.comp.no_hzb
		forward/mesh_cull_unified.comp.hzb
		forward/mdc_compact.comp
		forward/clear_count_buffers.comp
		# HZB occlusion pyramid build (P2 Stage A)
		hzb/downsample.comp
		forward/forward_mesh_vp.vert
		forward/fr_unlit.frag
		# Forward PBR / stylized fragment — multi-variant via LUX_SHADOW_SAMPLER_HEADER.
		forward/fr_pbr.frag.pcf
		forward/fr_pbr.frag.evsm
		forward/fr_stylized.frag.pcf
		forward/fr_stylized.frag.evsm
		# Shadow — mesh_shadow_vp.vert is the "fat" _vp vert with vUV +
		# vShadowNear/Far/DepthPersp varyings, paired with shadow_evsm_caster.frag.
		# shadow_depth_vp.vert is the thin _vp vUV-only vert paired with
		# shadow_depth.frag (PCF / depth-only). Splitting them avoids the
		# validation layer's OutputNotConsumed warning that hits when the fat
		# vert is forced onto the thin depth-only frag, while still using the
		# bindless pool path on both (the legacy attribute-path shadow_depth.vert
		# in this folder is dead code from before P3.2 R6 and is intentionally
		# NOT embedded — see the compile-list comment above).
		shadow/mesh_shadow_vp.vert
		shadow/shadow_depth_vp.vert
		shadow/shadow_depth.frag
		# EVSM technique shaders (C3)
		shadow/shadow_evsm_caster.frag
		shadow/shadow_evsm_blur_h.comp
		shadow/shadow_evsm_blur_v.comp
		# Skinning (1)
		skinning/skin_compute.comp
		# Skybox (3)
		skybox/skybox.vert
		skybox/skybox_cubemap.frag
		skybox/skybox_equirect.frag
		# Tonemap (2)
		postprocess/tonemap.vert
		postprocess/tonemap.frag
		# Gizmo (4)
		gizmo/line_list.vert
		gizmo/line_list.frag
		gizmo/tri_overlay.vert
		gizmo/tri_overlay.frag
		# Canvas2D (2)
		canvas2d/sprite.vert
		canvas2d/sprite.frag
		# Highlight outline
		highlight/highlight_mask.vert
		highlight/highlight_mask.frag
		highlight/highlight_blur.frag
		highlight/highlight_composite.frag
)

# Textures: separate asset pack (different type and source directory)
add_texture_asset(
	NAME	render_textures
	OUT_DIR	${ASSET_GENERATE_PATH}
    TEXTURE_FORMAT	bc3_srgb
    TEXTURE_COLOR_SPACE	srgb
	ENTRIES
        "${TEXTURE_SOURCE_PATH}/blue_nebulae_1.png|textures/blue_nebulae_1.luxasset"
        "${TEXTURE_SOURCE_PATH}/hazy_nebulae_1.png|textures/hazy_nebulae_1.luxasset"
        "${TEXTURE_SOURCE_PATH}/plain_starfield_1.png|textures/plain_starfield_1.luxasset"
        "${TEXTURE_SOURCE_PATH}/ringed_gas_giant_planet.png|textures/ringed_gas_giant_planet.luxasset"
)
target_add_asset(TARGET render NAME render_textures)
component_register_runtime_assets(
    COMPONENT render
    PATHS
        ${ASSET_GENERATE_PATH}/shaders
        ${ASSET_GENERATE_PATH}/textures
)

target_link_libraries(
    render
    PUBLIC
    lux::cxx::container
    lux::cxx::concurrent
    lux::engine::platform::window
    lux::engine::platform::gapi
    lux::engine::platform::common
    lux::engine::core::math
    # asset: kept for build-time texture-asset packing scripts (include_component_cmake_scripts
    # above + add_texture_asset / component_register_runtime_assets below). render uses no
    # asset RUNTIME symbols, but the CMake packaging integration needs the component.
    lux::engine::resource::asset
    # description: rdesc:: material/mesh/shader/texture descriptors (used ~191x in render).
    # Declared explicitly instead of riding in transitively through asset, so the dependency
    # graph reflects what render actually needs.
    lux::engine::resource::description
    PRIVATE
    meshoptimizer::meshoptimizer
    GPUOpen::VulkanMemoryAllocator
)

# ---------------------------------------------------------------------------
# Reflection code generation — EDITOR-ONLY sidecar (mirrors gameplay_meta).
#
# Render features opt into the feature-driven quality/tier system by declaring
# a reflectable LUX_CLASS param struct (e.g. TonemapParams). The editor
# enumerates those structs via ReflectionRegistry::findClass to build a generic
# settings panel. The generated .meta.cpp compile into a SEPARATE shared
# library `render_meta` that ONLY the editor links, so a shipped render.dll
# embeds no reflection symbols / type-name strings / registrar statics. Gated by
# LUX_BUILD_EDITOR: a no-editor build skips meta generation entirely.
# See .internal/feature-quality-tiers-design-2026-06-19.md.
# ---------------------------------------------------------------------------
if(LUX_BUILD_EDITOR)
    include_component_cmake_scripts(meta)

    # Sidecar SHARED lib FIRST so engine_enable_module_meta can attach the
    # generated sources. The anchor .cpp gives the generator a TU to derive
    # include paths / flags from; every other .cpp it ships is generated.
    add_library(render_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    set_target_properties(render_meta PROPERTIES
        OUTPUT_NAME lux_engine_function_render_meta)
    target_link_libraries(render_meta
        PRIVATE
            render                       # registration funcs reference render param types
            lux::engine::core::meta      # ReflectionRegistry / MetaModuleRegistrar
    )
    # Match the runtime module's exported-symbol macro so generated functions
    # are dllexport'ed from the sidecar.
    target_compile_definitions(render_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET         render
        SIDECAR_TARGET render_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/render/renderer/features/postprocess/TonemapParams.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/render/renderer/features/spatialcull/SpatialCullParams.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/render/renderer/features/shadow/ShadowQualityParams.hpp
    )
endif()

set(ENABLE_RENDER_TEST false CACHE BOOL "Enable render test")
if(ENABLE_RENDER_TEST)
    add_subdirectory(test)
endif()

