if(NOT LUX_BUILD_PACKED_RENDER_CONTENT)
    find_package(lux-cxx REQUIRED COMPONENTS compile_time)

    generate_visibility_header(
        ENABLE_MACRO_NAME       LUX_ENGINE_FUNCTION_RENDER_FEATURES_LIBRARY
        PUBLIC_MACRO_NAME       LUX_ENGINE_FUNCTION_RENDER_FEATURES_PUBLIC
        DISABLE_DLL_MACRO_NAME  LUX_ENGINE_FUNCTION_RENDER_FEATURES_DLL_DISABLE
        GENERATE_FILE_PATH      lux/engine/function/render/features/visibility.h
    )

    add_component(
        COMPONENT_NAME render_features
        NAMESPACE      lux::engine::function
        SOURCE_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/src/BuiltinContentProvider.cpp
    )
    component_include_directories(
        render_features
        BUILD_TIME_EXPORT
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${LUX_GENERATE_HEADER_DIR}
        INSTALL_TIME include
    )
    target_compile_definitions(
        render_features
        PRIVATE LUX_ENGINE_FUNCTION_RENDER_FEATURES_LIBRARY
    )
    target_link_libraries(
        render_features
        PUBLIC
            lux::engine::function::render_client
            lux::cxx::compile_time
    )
    component_add_internal_dependencies(render_features render_client)
    component_add_transitive_commands(
        render_features
        "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
        "find_package(lux-engine-function REQUIRED COMPONENTS render_client)"
    )
    lux_classify_target(
        TARGET  render_features
        LAYER   FUNCTION
        PRODUCT RUNTIME
        ROLE    DOMAIN
    )

    return()
endif()

find_package(Vulkan REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS core reflection_runtime)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_path(meshoptimizer_INCLUDE_DIRS NAMES meshoptimizer.h REQUIRED)

if(CMAKE_CROSSCOMPILING AND
   DEFINED ENV{VULKAN_SDK} AND
   EXISTS "$ENV{VULKAN_SDK}/Bin/glslc.exe")
    # find_package(Vulkan) reports the target triplet's glslc in a cross
    # configure. Shader compilation is a host build action, so that executable
    # is never runnable here.
    set(Vulkan_GLSLC_EXECUTABLE "$ENV{VULKAN_SDK}/Bin/glslc.exe")
elseif(NOT Vulkan_GLSLC_EXECUTABLE)
    lux_find_host_program(
        Vulkan_GLSLC_EXECUTABLE
        REQUIRED
        NAMES glslc glslc.exe
    )
endif()
message("---- GLSL Compiler path: ${Vulkan_GLSLC_EXECUTABLE}")

set(SHADER_SOURCE_PATH      ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders)
set(GLSL_SHADER_SOURCE_PATH ${SHADER_SOURCE_PATH}/glsl)
set(ASSET_GENERATE_PATH     ${CMAKE_BINARY_DIR}/assets)
set(TEXTURE_SOURCE_PATH     ${CMAKE_CURRENT_SOURCE_DIR}/assets/textures)

include_component_cmake_scripts(lux::cxx::reflection_runtime)
include_component_cmake_scripts(meta)
include_component_cmake_scripts(lux::engine::resource::asset)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/compile_glsl_shaders.cmake)

set(RENDER_FEATURE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/canvas2d/Canvas2DOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/deferred/DeferredGBufferOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/deferred/DeferredLightingOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/FeatureParamsHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/forward/ForwardMeshOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/gizmo/LineListOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/gizmo/TriOverlayOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/grid/Grid2DOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/grid/Grid3DOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/highlight/HighlightOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/light/LightOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/material/MaterialOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/meshstack/MeshStackOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/point_cloud/PCOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/postprocess/FogOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/render_cluster/RenderClusterOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/terrain/TerrainOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/shadow/MeshShadowOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/shadow/ShadowMapOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/skinning/SkinningOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/sky_box/SkyboxOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/spatialcull/SpatialCullOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/streaming_feedback/StreamingFeedbackOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/trajectory/TrajectoryOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/view_camera/ViewCameraOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/assembly/water/WaterOperationHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/MeshKernels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/ShadowKernels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/kernels/UtilityKernels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/BuiltinShaderContextView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/canvas2d/Canvas2DFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/deferred/DeferredGBufferFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/deferred/DeferredLightingFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/DepthPrepassFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/forward/ForwardMeshFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/gizmo/LineListTransientFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/gizmo/TriOverlayTransientFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/GpuDrivenMeshFeatureBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/grid/Grid2DPassFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/grid/Grid3DPassFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/highlight/HighlightFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/hzb/HzbFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/light/LightFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/material/StandardMaterialFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/meshstack/StandardMeshStackFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/point_cloud/PCFeatureGPUDriven.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/point_cloud/PCFeatureIndirectBase.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/point_cloud/PCFeatureLOD.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/point_cloud/PCFeatureSimple.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/point_cloud/PCFeatureSplatting.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/point_cloud/PCFeatureTransient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/render_cluster/RenderClusterFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/render_cluster/RenderClusterResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/terrain/TerrainFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/terrain/TerrainResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/postprocess/LinearDepthFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/postprocess/FogFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/postprocess/SsaoFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/postprocess/TonemapFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/shadow/EVSMShadowResources.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/shadow/EVSMShadowTechnique.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/shadow/MeshShadowFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/shadow/ShadowMapFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/skinning/SkinningFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/sky_box/SkyboxFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/spatialcull/SpatialCullFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/streaming_feedback/StreamingFeedbackFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/trajectory/TrajectoryLineFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/view_camera/StandardViewCameraFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/features/water/WaterFeature.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/scene/SpatialCullGrid.cpp
)

add_component(
    COMPONENT_NAME render_features
    NAMESPACE      lux::engine::function
    SOURCE_FILES   ${RENDER_FEATURE_SRCS}
)

component_include_directories(
    render_features
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/sinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/../client/include
        ${CMAKE_BINARY_DIR}/gen
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../vulkan/pinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
        ${meshoptimizer_INCLUDE_DIRS}
)

component_add_cmake_scripts(
    render_features
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/compile_glsl_shaders.cmake
)

target_compile_definitions(
    render_features
    PRIVATE
        LUX_FUNCTION_LIBRARY
)

if(WIN32)
    target_compile_definitions(
        render_features
        PRIVATE
            VK_USE_PLATFORM_WIN32_KHR
            NOMINMAX
    )
endif()

component_add_internal_dependencies(render_features render_vulkan)

component_add_transitive_commands(
    render_features
    "find_package(lux-cxx REQUIRED COMPONENTS core)"
)

lux_classify_target(
        TARGET  render_features
        LAYER   FUNCTION
        PRODUCT RUNTIME
        ROLE    DOMAIN
    )

target_link_libraries(
    render_features
    PRIVATE
        lux::cxx::core
        meshoptimizer::meshoptimizer
        GPUOpen::VulkanMemoryAllocator
)

# ---------------------------------------------------------------------------
# Shader pipeline: compile GLSL -> SPIR-V -> pack .luxasset
# RENDER_SHADER_SPVS receives intermediate .spv paths for build-time use only.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# PassParams 构建钟(L2-1):一份注解头 → 生成 .pass.hpp(C++ 侧)+ .lglslh
# (GLSL 侧),两侧同源。生成的 .lglslh 由下方第二个 emit_lglsl_shaders 调用
# 注入 set/binding 后进入统一的 lglsl_emitted 树，不受 Editor 产品选择门控：
# 这是运行时着色器管线的一部分,不是编辑器反射。
# ---------------------------------------------------------------------------
include_component_cmake_scripts(meta)

# 解析这三个头需要 <vulkan/vulkan.h>。目标本身通过 Vulkan::Vulkan 拿到该路径,
# 但 meta_generator 是**独立进程**,只吃我们显式传给它的 -I —— 不给的话它报
# "'vulkan/vulkan.h' file not found",而且**不影响退出码**:构建照常成功,只是
# 这三个头的 pass 参数没被解析出来。
#
# 不能直接用 Vulkan_INCLUDE_DIRS:交叉配置下 find_package(Vulkan) 找到的是
# **NDK sysroot 的整个 usr/include**,把它喂给解析器等于把 NDK 的 libc 一起灌
# 进去,于是 stdint.h/cdefs.h 与解析器自带的宿主头互相打架(typedef 重定义、
# 找不到 asm/types.h)。解析是**宿主活动**,要的是平台中立的那份声明。
#
# 所以显式找 Vulkan SDK 的 include。Vulkan 头本身平台无关(平台差异都在
# VK_USE_PLATFORM_* 宏里,解析期不需要),这样宿主与交叉两条路解析出的结构
# 也保证一致。
set(_pass_parse_vulkan_inc "")
if(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/include/vulkan/vulkan.h")
    set(_pass_parse_vulkan_inc "$ENV{VULKAN_SDK}/include")
elseif(NOT CMAKE_CROSSCOMPILING AND Vulkan_INCLUDE_DIRS)
    # 宿主上 find_package 找到的就是 SDK 那份,直接用
    list(GET Vulkan_INCLUDE_DIRS 0 _pass_parse_vulkan_inc)
endif()

if(NOT _pass_parse_vulkan_inc)
    message(FATAL_ERROR
        "[pass-params] 找不到平台中立的 Vulkan 头用于解析 —— meta_generator 将无法\n"
        "解析 PassParams 头,而**这不会让构建失败**,只会静默少生成 pass 参数。\n"
        "设置 VULKAN_SDK 指向 Vulkan SDK 根目录。")
endif()
message(STATUS "[pass-params] 解析用 Vulkan 头: ${_pass_parse_vulkan_inc}")

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/engine_render_codegen.cmake)

engine_add_pass_params(
    NAME         render_highlight_pass_params
    TARGET       render_features
    TARGET_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude/lux/engine/render/renderer/features/highlight/HighlightBlurPassParams.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude/lux/engine/render/renderer/features/highlight/HighlightCompositePassParams.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude/lux/engine/render/renderer/features/streaming_feedback/StreamingFeedbackPassParams.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude/lux/engine/render/renderer/features/postprocess/TonemapPassParams.hpp
    EXTRA_COMPILE_OPTIONS
        -I${_pass_parse_vulkan_inc}
    OUT_DIR_VAR  RENDER_PASS_GEN_DIR
)

# ---------------------------------------------------------------------------
# 通信外观构建钟(R3-3 A+):render_client 已经生成并安装 .ops.hpp
# 与 .client.ops.cpp；此处只生成需要 Vulkan 特性实现的服务端 .ops.cpp。
# 手写残余=handle<Op> 语义；非同构 createFn 仍用 custom_create=true 逃生舱。
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/../client/cmake/render_comm_operations.cmake)

engine_add_comm_ops(
    NAME                  render_comm_ops
    IMPLEMENTATION_TARGET render_features
    INCLUDE_PREFIX        lux/engine/function/render/client/genops
    TARGET_FILES          ${LUX_RENDER_COMM_OPERATION_HEADERS}
)

# ---------------------------------------------------------------------------
# The .lglsl pipeline: the emitter injects the canonical layout per LayoutContract.
# Emission must happen before any GLSL compilation — the emitted tree is the
# include directory for all of them (now that shared headers have moved to
# .lglslh, the real body behind an `include "foo.glsl"` lives in the emitted
# tree); EXTRA_DEPENDS makes changes to .lglslh correctly trigger a rebuild of
# consumers. Design: see descriptor-layout-architecture §4.4.
# ---------------------------------------------------------------------------
emit_lglsl_shaders(
	EMIT_TARGET		render_lglsl_emit
	SOURCE_DIR		${SHADER_SOURCE_PATH}
	OUTPUT_DIR		LGLSL_EMITTED_DIR
	OUTPUT_FILES	LGLSL_EMITTED_FILES
	SOURCES
		view_pc_prefix.lglslh
		cluster_common.lglslh
		shadow_common.lglslh
		lighting_common.lglslh
		postprocess/tonemap.frag.lglsl
		postprocess/linear_depth.frag.lglsl
		postprocess/ssao.frag.lglsl
		postprocess/fog.frag.lglsl
		water/water.frag.lglsl
		grid/grid.vert.lglsl
		grid/grid.frag.lglsl
		grid/grid2d.frag.lglsl
		gizmo/line_list.vert.lglsl
		gizmo/line_list.frag.lglsl
		gizmo/tri_overlay.vert.lglsl
		trajectory/trajectory_line.vert.lglsl
		trajectory/trajectory_line.frag.lglsl
		skybox/skybox.vert.lglsl
		skybox/skybox_cubemap.frag.lglsl
		skybox/skybox_equirect.frag.lglsl
		postprocess/tonemap.vert.lglsl
		depth_prepass/depth_only.frag.lglsl
		canvas2d/image.vert.lglsl
		canvas2d/image.frag.lglsl
		canvas2d/pixel_field.vert.lglsl
		canvas2d/pixel_field.frag.lglsl
		canvas2d/tile.vert.lglsl
		canvas2d/tile.frag.lglsl
		canvas2d/group_composite.frag.lglsl
		highlight/highlight_blur.frag.lglsl
		highlight/highlight_composite.frag.lglsl
		streaming_feedback/streaming_mask.frag.lglsl
		streaming_feedback/streaming_composite.frag.lglsl
		point_cloud/pointcloud_simple.vert.lglsl
		point_cloud/pointcloud_simple.frag.lglsl
		point_cloud/pointcloud_lod.vert.lglsl
		point_cloud/pointcloud_splat.frag.lglsl
		forward/fr_unlit.frag.lglsl
		forward/forward_mesh_vp.vert.lglsl
		deferred/gbuffer_vp.vert.lglsl
		render_cluster/pick.vert.lglsl
		render_cluster/pick.frag.lglsl
		terrain/patch.vert.lglsl
		terrain/patch.frag.lglsl
		deferred/gbuffer_pbr.frag.lglsl
		deferred/gbuffer_unlit.frag.lglsl
		deferred/gbuffer_stylized.frag.lglsl
		highlight/highlight_mask.vert.lglsl
		highlight/highlight_mask.frag.lglsl
		forward/fr_pbr.frag.lglsl
		forward/fr_stylized.frag.lglsl
)

# 生成的 PassParams .lglslh(pass_gen 目录)走同一发射器,--header 注入
# set/binding 后落进同一 lglsl_emitted 树 —— include 名即 <Stem>.glsl,
# 现有 INCLUDE_DIRS 无需扩;EXTRA_DEPENDS 追加钉住构建序。
emit_lglsl_shaders(
	EMIT_TARGET		render_pass_params_emit
	SOURCE_DIR		${RENDER_PASS_GEN_DIR}
	OUTPUT_DIR		LGLSL_PASSGEN_EMITTED_DIR
	OUTPUT_FILES	LGLSL_PASSGEN_EMITTED_FILES
	SOURCES
		HighlightBlurPassParams.lglslh
		HighlightCompositePassParams.lglslh
		StreamingFeedbackPassParams.lglslh
		TonemapPassParams.lglslh
)

compile_and_pack_shaders(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render_features
	SHADER_DIR		${SHADER_SOURCE_PATH}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${LGLSL_EMITTED_DIR} ${SHADER_SOURCE_PATH}
	EXTRA_DEPENDS	${LGLSL_EMITTED_FILES}
	SHADERS
		# Grid


		# Depth prepass

		# Point cloud


		# Point cloud compute
		point_cloud/pointcloud_culling.comp
		# GPU-driven (unified). mesh_cull_unified.comp moved to multi-variant
		# (no_hzb / hzb) compile below — see compile_and_pack_shader_variants.
        forward/mdc_compact.comp
        forward/clear_count_buffers.comp
		# HZB occlusion pyramid build (P2 Stage A)
		hzb/downsample.comp
		# Render-cluster coarse cull and candidate expansion. Raster picking is
		# emitted through the reflected .lglsl path below.
		render_cluster/cluster_cull.comp
		render_cluster/candidate_expand.comp
		terrain/patch_select.comp
		# P3.2 R6: single mesh vertex shader path (the _vp variant; legacy
		# attribute-path forward_mesh.vert / mesh_shadow.vert deleted).

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

		# Shading models (require INCLUDE_DIRS for brdf/ and lighting headers)
		# NOTE: fr_pbr.frag and fr_stylized.frag are compiled below as
		# multi-variant (PCF / EVSM) via compile_and_pack_shader_variants —
		# they include the shadow technique impl indirectly through
		# LUX_SHADOW_SAMPLER_HEADER.

		# Trajectory

		# Shadow mapping
		shadow/shadow_depth.frag
		# EVSM technique (C3): moment caster + separable Gaussian blur.
		shadow/shadow_evsm_caster.frag
		shadow/shadow_evsm_blur_h.comp
		shadow/shadow_evsm_blur_v.comp
		# Deferred rendering — P3.2 R6: single _vp gbuffer vertex shader.



		# deferred_lighting.frag is compiled below via compile_and_pack_shader_variants
		# (PCF / EVSM variants of LUX_SHADOW_SAMPLER_HEADER).
		deferred/deferred_lighting.vert
		deferred/cluster_build.comp
		deferred/cluster_count.comp
		deferred/cluster_scan.comp
		deferred/cluster_fill.comp
		# Post-processing (tonemap.frag has moved to .lglsl; see the lglsl pipeline below)

		# Gizmo


		# Canvas2D (GPU-driven 2D instances: image + pixel-field kinds)




		# Highlight outline


)

compile_and_pack_shaders(
	COMPILE_TARGET	render_lglsl_shaders
	ATTACH_TO		render_features
	SHADER_DIR		${LGLSL_EMITTED_DIR}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_LGLSL_SPVS
	INCLUDE_DIRS	${LGLSL_EMITTED_DIR} ${SHADER_SOURCE_PATH}
	# PassParams 生成头的发射产物也在依赖里:highlight_blur/composite include 它们
	EXTRA_DEPENDS	${LGLSL_EMITTED_FILES} ${LGLSL_PASSGEN_EMITTED_FILES}
	SHADERS
		postprocess/tonemap.frag
		postprocess/linear_depth.frag
		postprocess/ssao.frag
		postprocess/fog.frag
		water/water.frag
		grid/grid.vert
		grid/grid.frag
		grid/grid2d.frag
		gizmo/line_list.vert
		gizmo/line_list.frag
		gizmo/tri_overlay.vert
		trajectory/trajectory_line.vert
		trajectory/trajectory_line.frag
		skybox/skybox.vert
		skybox/skybox_cubemap.frag
		skybox/skybox_equirect.frag
		postprocess/tonemap.vert
		depth_prepass/depth_only.frag
		canvas2d/image.vert
		canvas2d/image.frag
		canvas2d/pixel_field.vert
		canvas2d/pixel_field.frag
		canvas2d/tile.vert
		canvas2d/tile.frag
		canvas2d/group_composite.frag
		highlight/highlight_blur.frag
		highlight/highlight_composite.frag
		streaming_feedback/streaming_mask.frag
		streaming_feedback/streaming_composite.frag
		point_cloud/pointcloud_simple.vert
		point_cloud/pointcloud_simple.frag
		point_cloud/pointcloud_lod.vert
		point_cloud/pointcloud_splat.frag
		forward/fr_unlit.frag
		forward/forward_mesh_vp.vert
		deferred/gbuffer_vp.vert
		render_cluster/pick.vert
		render_cluster/pick.frag
		terrain/patch.vert
		terrain/patch.frag
		deferred/gbuffer_pbr.frag
		deferred/gbuffer_unlit.frag
		deferred/gbuffer_stylized.frag
		highlight/highlight_mask.vert
		highlight/highlight_mask.frag
)
list(APPEND RENDER_SHADER_SPVS ${RENDER_LGLSL_SPVS})

# ---------------------------------------------------------------------------
# Shadow technique variants — single .frag source compiled with different
# LUX_SHADOW_SAMPLER_HEADER injections (PCF / EVSM / future MSM, VSM ...).
# See .internal/plan/evsm-shadow-implementation-guide.md.
# ---------------------------------------------------------------------------
set(_shadow_variants
	"pcf|LUX_SHADOW_TECHNIQUE_ID=0"
	"evsm|LUX_SHADOW_TECHNIQUE_ID=1"
)
# Deferred lighting multiplies the shadow axis with the G-buffer READ axis
# (line-B P3): sampled (classic two-scope) × local_read (tile-local merged
# scope, KHR_dynamic_rendering_local_read). subpassLoad is a different SPIR-V
# resource type, so this MUST be a baked variant, not a spec constant.
set(_lighting_variants
	"pcf|LUX_SHADOW_TECHNIQUE_ID=0"
	"evsm|LUX_SHADOW_TECHNIQUE_ID=1"
	"pcf_lr|LUX_SHADOW_TECHNIQUE_ID=0,LUX_GBUFFER_LOCAL_READ=1"
	"evsm_lr|LUX_SHADOW_TECHNIQUE_ID=1,LUX_GBUFFER_LOCAL_READ=1"
)
compile_and_pack_shader_variants(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render_features
	SHADER_DIR		${SHADER_SOURCE_PATH}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${LGLSL_EMITTED_DIR} ${SHADER_SOURCE_PATH}
	EXTRA_DEPENDS	${LGLSL_EMITTED_FILES}
	SHADER			deferred/deferred_lighting.frag
	VARIANTS		${_lighting_variants}
)
compile_and_pack_shader_variants(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render_features
	SHADER_DIR		${LGLSL_EMITTED_DIR}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${LGLSL_EMITTED_DIR} ${SHADER_SOURCE_PATH}
	EXTRA_DEPENDS	${LGLSL_EMITTED_FILES}
	SHADER			forward/fr_pbr.frag
	VARIANTS		${_shadow_variants}
)
compile_and_pack_shader_variants(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render_features
	SHADER_DIR		${LGLSL_EMITTED_DIR}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${LGLSL_EMITTED_DIR} ${SHADER_SOURCE_PATH}
	EXTRA_DEPENDS	${LGLSL_EMITTED_FILES}
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
	"no_hzb_paged|LUX_INSTANCE_PAGED=1"
	"hzb_paged|HZB_ENABLED=1,LUX_INSTANCE_PAGED=1"
)
compile_and_pack_shader_variants(
	COMPILE_TARGET	render_shaders
	ATTACH_TO		render_features
	SHADER_DIR		${SHADER_SOURCE_PATH}
	ASSET_OUT_DIR	${ASSET_GENERATE_PATH}
	OUTPUT_SPIR_V	RENDER_SHADER_SPVS
	INCLUDE_DIRS	${LGLSL_EMITTED_DIR} ${SHADER_SOURCE_PATH}
	EXTRA_DEPENDS	${LGLSL_EMITTED_FILES}
	SHADER			forward/mesh_cull_unified.comp
	VARIANTS		${_cull_hzb_variants}
)

# BDA instance-page variants. Flat shaders remain the Legacy feature-level
# implementation; devices with BDA+shaderInt64 resolve these variants under
# the same feature contracts.
foreach(_paged_lglsl_shader
	forward/forward_mesh_vp.vert
	deferred/gbuffer_vp.vert
	highlight/highlight_mask.vert
)
	compile_and_pack_shader_variants(
		COMPILE_TARGET render_shaders
		ATTACH_TO render_features
		SHADER_DIR ${LGLSL_EMITTED_DIR}
		ASSET_OUT_DIR ${ASSET_GENERATE_PATH}
		OUTPUT_SPIR_V RENDER_SHADER_SPVS
		INCLUDE_DIRS ${LGLSL_EMITTED_DIR} ${SHADER_SOURCE_PATH}
		EXTRA_DEPENDS ${LGLSL_EMITTED_FILES}
		SHADER ${_paged_lglsl_shader}
		VARIANTS "paged|LUX_INSTANCE_PAGED=1"
	)
endforeach()

foreach(_paged_shadow_shader
	shadow/mesh_shadow_vp.vert
	shadow/shadow_depth_vp.vert
)
	compile_and_pack_shader_variants(
		COMPILE_TARGET render_shaders
		ATTACH_TO render_features
		SHADER_DIR ${SHADER_SOURCE_PATH}
		ASSET_OUT_DIR ${ASSET_GENERATE_PATH}
		OUTPUT_SPIR_V RENDER_SHADER_SPVS
		INCLUDE_DIRS ${LGLSL_EMITTED_DIR} ${SHADER_SOURCE_PATH}
		EXTRA_DEPENDS ${LGLSL_EMITTED_FILES}
		SHADER ${_paged_shadow_shader}
		VARIANTS "paged|LUX_INSTANCE_PAGED=1"
	)
endforeach()

# ---------------------------------------------------------------------------
# Embed built-in feature shaders as constexpr C++ byte arrays
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_builtin_shaders.cmake)
embed_builtin_shaders(
	TARGET		render_features
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
		# Grid (3)
		grid/grid.vert
		grid/grid.frag
		grid/grid2d.frag
		# Trajectory (2)
		trajectory/trajectory_line.vert
		trajectory/trajectory_line.frag
		# Deferred GBuffer — P3.2 R6: single _vp vertex shader.
		deferred/gbuffer_vp.vert
		deferred/gbuffer_vp.vert.paged
		deferred/gbuffer_unlit.frag
		deferred/gbuffer_pbr.frag
		deferred/gbuffer_stylized.frag
		# Deferred Lighting (vert + clustered; .frag is variant-baked below)
		deferred/deferred_lighting.vert
		deferred/cluster_build.comp
		deferred/cluster_count.comp
		deferred/cluster_scan.comp
		deferred/cluster_fill.comp
		# Deferred Lighting fragment — shadow technique × G-buffer read mode
		# (line-B P3: _lr = tile-local subpassLoad variants).
		deferred/deferred_lighting.frag.pcf
		deferred/deferred_lighting.frag.evsm
		deferred/deferred_lighting.frag.pcf_lr
		deferred/deferred_lighting.frag.evsm_lr
		# Forward / shared compute — P3.2 R6: single _vp mesh vertex shader.
		forward/mesh_cull_unified.comp.no_hzb
		forward/mesh_cull_unified.comp.hzb
		forward/mesh_cull_unified.comp.no_hzb_paged
		forward/mesh_cull_unified.comp.hzb_paged
		forward/mdc_compact.comp
		forward/clear_count_buffers.comp
		# HZB occlusion pyramid build (P2 Stage A)
		hzb/downsample.comp
		render_cluster/cluster_cull.comp
		render_cluster/candidate_expand.comp
		render_cluster/pick.vert
		render_cluster/pick.frag
		terrain/patch_select.comp
		terrain/patch.vert
		terrain/patch.frag
		forward/forward_mesh_vp.vert
		forward/forward_mesh_vp.vert.paged
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
		shadow/mesh_shadow_vp.vert.paged
		shadow/shadow_depth_vp.vert
		shadow/shadow_depth_vp.vert.paged
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
		# Tonemap + LinearDepth + Ssao + Fog (5)
		postprocess/tonemap.vert
		postprocess/tonemap.frag
		postprocess/linear_depth.frag
		postprocess/ssao.frag
		postprocess/fog.frag
		water/water.frag
		# Gizmo (3) — tri_overlay.frag deleted; TRI_OVERLAY_FRAG aliases line_list.frag (R-3)
		gizmo/line_list.vert
		gizmo/line_list.frag
		gizmo/tri_overlay.vert
		# Canvas2D (4)
		canvas2d/image.vert
		canvas2d/image.frag
		canvas2d/pixel_field.vert
		canvas2d/pixel_field.frag
		canvas2d/tile.vert
		canvas2d/tile.frag
		canvas2d/group_composite.frag
		# Highlight outline
		highlight/highlight_mask.vert
		highlight/highlight_mask.vert.paged
		highlight/highlight_mask.frag
		highlight/highlight_blur.frag
		highlight/highlight_composite.frag
		streaming_feedback/streaming_mask.frag
		streaming_feedback/streaming_composite.frag
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
target_add_asset(TARGET render_features NAME render_textures)
component_register_runtime_assets(
    COMPONENT render_features
    PATHS
        ${ASSET_GENERATE_PATH}/shaders
        ${ASSET_GENERATE_PATH}/textures
)

# ---------------------------------------------------------------------------
# Reflection code generation — EDITOR-ONLY sidecar (mirrors gameplay_meta).
#
# Render features opt into the feature-driven quality/tier system by declaring
# a runtime-reflectable parameter struct (e.g. TonemapParams). The editor
# enumerates those structs via ReflectionRegistry::findClass to build a generic
# settings panel. The generated .meta.cpp compile into a SEPARATE shared
# library `render_meta` that ONLY the editor links, so shipped runtime render
# components embed no reflection symbols / type-name strings / registrar statics.
# Editor-only parameter reflection sidecar.
# See .internal/feature-quality-tiers-design-2026-06-19.md.
# ---------------------------------------------------------------------------
if(LUX_PROFILE_HAS_EDITOR)
    include_component_cmake_scripts(meta)

    # Sidecar SHARED lib FIRST so engine_enable_module_meta can attach the
    # generated sources. The anchor .cpp gives the generator a TU to derive
    # include paths / flags from; every other .cpp it ships is generated.
    add_library(render_meta SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
    lux_classify_target(
        TARGET render_meta
        LAYER FUNCTION
        PRODUCT EDITOR
        ROLE INTEGRATION
    )
    set_target_properties(render_meta PROPERTIES
        OUTPUT_NAME lux_engine_function_render_meta)
    target_link_libraries(render_meta
        PRIVATE
            render_features              # registration funcs reference feature param types
            lux::engine::core::meta      # ReflectionRegistry / MetaModuleRegistrar
    )
    # Match the runtime module's exported-symbol macro so generated functions
    # are dllexport'ed from the sidecar.
    target_compile_definitions(render_meta PRIVATE LUX_FUNCTION_LIBRARY)

    engine_enable_module_meta(
        TARGET         render_features
        SIDECAR_TARGET render_meta
        TARGET_FILES
            ${CMAKE_CURRENT_SOURCE_DIR}/../client/include/lux/engine/function/render/client/features/postprocess/TonemapParams.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../client/include/lux/engine/function/render/client/features/spatialcull/SpatialCullParams.hpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../client/include/lux/engine/function/render/client/features/shadow/ShadowQualityParams.hpp
    )
endif()
