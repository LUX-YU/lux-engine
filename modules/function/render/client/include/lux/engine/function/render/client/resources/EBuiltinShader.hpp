#pragma once
/**
 * @file EBuiltinShader.hpp
 * @brief Public identifiers for the engine's built-in (embedded) shaders.
 *
 * The EBuiltinShader enum + its driving X-macro list are split out of the
 * module-private pinclude BuiltinShaderRegistry.hpp (which keeps the generated
 * *_embed.hpp byte arrays + getBuiltinShader/ensureBuiltinShader) so the public
 * SDK surface (RenderContextView::createBuiltinShaderModule) can NAME a built-in shader
 * without pulling the build-time embed artifacts. Pure tokens — depends only on
 * <cstdint>.
 *
 * To add a new built-in shader:
 *   1. add the .luxasset path to the SHADERS list in CMakeLists.txt
 *      (under embed_builtin_shaders) — generates a matching `<prefix>_embed.hpp`
 *      and byte arrays in `lux::render::builtin`.
 *   2. add one X(...) row below.
 *   The enum and the dispatch in BuiltinShaderRegistry.hpp are both derived from
 *   this list, so they stay in sync by construction.
 */

#include <cstdint>

namespace lux::render
{
    // ----------------------------------------------------------------------
    //  X-macro list of all built-in shaders.
    //  Each entry maps an EBuiltinShader enum value to the embed-name prefix
    //  emitted by lux_asset_packer (which generates `<prefix>_spirv[]`,
    //  `<prefix>_spirv_size`, `<prefix>_info[]`, `<prefix>_info_size`).
    // ----------------------------------------------------------------------
#define LUX_BUILTIN_SHADER_LIST(X)                                                                                     \
    /* Point cloud */                                                                                                  \
    X(PC_SIMPLE_VERT, point_cloud_pointcloud_simple_vert)                                                              \
    X(PC_SIMPLE_FRAG, point_cloud_pointcloud_simple_frag)                                                              \
    X(PC_LOD_VERT, point_cloud_pointcloud_lod_vert)                                                                    \
    X(PC_SPLAT_FRAG, point_cloud_pointcloud_splat_frag)                                                                \
    X(PC_CULLING_COMP, point_cloud_pointcloud_culling_comp)                                                            \
    /* Grid */                                                                                                         \
    X(GRID_VERT, grid_grid_vert)                                                                                       \
    X(GRID_FRAG, grid_grid_frag)                                                                                       \
    X(GRID2D_FRAG, grid_grid2d_frag)                                                                                   \
    /* Trajectory */                                                                                                   \
    X(TRAJECTORY_LINE_VERT, trajectory_trajectory_line_vert)                                                           \
    X(TRAJECTORY_LINE_FRAG, trajectory_trajectory_line_frag)                                                           \
    /* Deferred GBuffer (GBUFFER_VERT serves the only _vp variant) */                                                  \
    X(GBUFFER_VERT, deferred_gbuffer_vp_vert)                                                                          \
    X(GBUFFER_VERT_PAGED, deferred_gbuffer_vp_vert_paged)                                                              \
    X(GBUFFER_UNLIT_FRAG, deferred_gbuffer_unlit_frag)                                                                 \
    X(GBUFFER_PBR_FRAG, deferred_gbuffer_pbr_frag)                                                                     \
    X(GBUFFER_STYLIZED_FRAG, deferred_gbuffer_stylized_frag)                                                           \
    /* Deferred Lighting (.frag has one entry per shadow technique) */                                                 \
    X(DEFERRED_LIGHTING_VERT, deferred_deferred_lighting_vert)                                                         \
    X(DEFERRED_LIGHTING_FRAG_PCF, deferred_deferred_lighting_frag_pcf)                                                 \
    X(DEFERRED_LIGHTING_FRAG_EVSM, deferred_deferred_lighting_frag_evsm)                                               \
    X(DEFERRED_LIGHTING_FRAG_PCF_LR, deferred_deferred_lighting_frag_pcf_lr)                                           \
    X(DEFERRED_LIGHTING_FRAG_EVSM_LR, deferred_deferred_lighting_frag_evsm_lr)                                         \
    X(CLUSTER_BUILD_COMP, deferred_cluster_build_comp)                                                                 \
    X(CLUSTER_COUNT_COMP, deferred_cluster_count_comp)                                                                 \
    X(CLUSTER_SCAN_COMP, deferred_cluster_scan_comp)                                                                   \
    X(CLUSTER_FILL_COMP, deferred_cluster_fill_comp)                                                                   \
    /* Forward / shared compute (FORWARD_MESH_VERT serves the only _vp variant) */                                     \
    X(MESH_CULL_UNIFIED_COMP, forward_mesh_cull_unified_comp_no_hzb)                                                   \
    X(MESH_CULL_UNIFIED_COMP_HZB, forward_mesh_cull_unified_comp_hzb)                                                  \
    X(MESH_CULL_UNIFIED_COMP_PAGED, forward_mesh_cull_unified_comp_no_hzb_paged)                                       \
    X(MESH_CULL_UNIFIED_COMP_HZB_PAGED, forward_mesh_cull_unified_comp_hzb_paged)                                      \
    X(MDC_COMPACT_COMP, forward_mdc_compact_comp)                                                                      \
    X(CLEAR_COUNT_BUFFERS_COMP, forward_clear_count_buffers_comp)                                                      \
    /* HZB occlusion pyramid (P2 Stage A) */                                                                           \
    X(HZB_DOWNSAMPLE_COMP, hzb_downsample_comp)                                                                        \
    X(RENDER_CLUSTER_CULL_COMP, render_cluster_cluster_cull_comp)                                                      \
    X(RENDER_CLUSTER_EXPAND_COMP, render_cluster_candidate_expand_comp)                                                \
    X(RENDER_CLUSTER_PICK_VERT, render_cluster_pick_vert)                                                              \
    X(RENDER_CLUSTER_PICK_FRAG, render_cluster_pick_frag)                                                              \
    X(TERRAIN_PATCH_SELECT_COMP, terrain_patch_select_comp)                                                            \
    X(TERRAIN_PATCH_VERT, terrain_patch_vert)                                                                          \
    X(TERRAIN_PATCH_FRAG, terrain_patch_frag)                                                                          \
    X(FORWARD_MESH_VERT, forward_forward_mesh_vp_vert)                                                                 \
    X(FORWARD_MESH_VERT_PAGED, forward_forward_mesh_vp_vert_paged)                                                     \
    X(FORWARD_UNLIT_FRAG, forward_fr_unlit_frag)                                                                       \
    X(FORWARD_PBR_FRAG_PCF, forward_fr_pbr_frag_pcf)                                                                   \
    X(FORWARD_PBR_FRAG_EVSM, forward_fr_pbr_frag_evsm)                                                                 \
    X(FORWARD_STYLIZED_FRAG_PCF, forward_fr_stylized_frag_pcf)                                                         \
    X(FORWARD_STYLIZED_FRAG_EVSM, forward_fr_stylized_frag_evsm)                                                       \
    /* Shadow — MESH_SHADOW_VERT is the EVSM-caster vert (vUV + vShadowNear/Far/DepthPersp);                         \
     * SHADOW_DEPTH_VERT is the depth-only vert (vUV only) paired with shadow_depth.frag.                              \
     * Both verts use the bindless pool path; only their output interpolant sets differ.   */                          \
    X(MESH_SHADOW_VERT, shadow_mesh_shadow_vp_vert)                                                                    \
    X(MESH_SHADOW_VERT_PAGED, shadow_mesh_shadow_vp_vert_paged)                                                        \
    X(SHADOW_DEPTH_VERT, shadow_shadow_depth_vp_vert)                                                                  \
    X(SHADOW_DEPTH_VERT_PAGED, shadow_shadow_depth_vp_vert_paged)                                                      \
    X(SHADOW_DEPTH_FRAG, shadow_shadow_depth_frag)                                                                     \
    X(SHADOW_EVSM_CASTER_FRAG, shadow_shadow_evsm_caster_frag)                                                         \
    X(SHADOW_EVSM_BLUR_H_COMP, shadow_shadow_evsm_blur_h_comp)                                                         \
    X(SHADOW_EVSM_BLUR_V_COMP, shadow_shadow_evsm_blur_v_comp)                                                         \
    /* Skybox */                                                                                                       \
    X(SKYBOX_VERT, skybox_skybox_vert)                                                                                 \
    X(SKYBOX_CUBEMAP_FRAG, skybox_skybox_cubemap_frag)                                                                 \
    X(SKYBOX_EQUIRECT_FRAG, skybox_skybox_equirect_frag)                                                               \
    /* Tonemap */                                                                                                      \
    X(TONEMAP_VERT, postprocess_tonemap_vert)                                                                          \
    X(TONEMAP_FRAG, postprocess_tonemap_frag)                                                                          \
    X(LINEAR_DEPTH_FRAG, postprocess_linear_depth_frag)                                                                \
    X(SSAO_FRAG, postprocess_ssao_frag)                                                                                \
    X(FOG_FRAG, postprocess_fog_frag)                                                                                  \
    X(WATER_FRAG, water_water_frag)                                                                                    \
    /* Gizmo */                                                                                                        \
    X(LINE_LIST_VERT, gizmo_line_list_vert)                                                                            \
    X(LINE_LIST_FRAG, gizmo_line_list_frag)                                                                            \
    X(TRI_OVERLAY_VERT, gizmo_tri_overlay_vert)                                                                        \
    X(TRI_OVERLAY_FRAG, gizmo_line_list_frag)                                                                          \
    /* Canvas2D (GPU-driven 2D instances → SceneColor) */                                                              \
    X(CANVAS2D_IMAGE_VERT, canvas2d_image_vert)                                                                        \
    X(CANVAS2D_IMAGE_FRAG, canvas2d_image_frag)                                                                        \
    X(CANVAS2D_FIELD_VERT, canvas2d_pixel_field_vert)                                                                  \
    X(CANVAS2D_FIELD_FRAG, canvas2d_pixel_field_frag)                                                                  \
    X(CANVAS2D_TILE_VERT, canvas2d_tile_vert)                                                                          \
    X(CANVAS2D_TILE_FRAG, canvas2d_tile_frag)                                                                          \
    X(CANVAS2D_GROUP_COMPOSITE_FRAG, canvas2d_group_composite_frag)                                                    \
    /* Highlight outline (object highlight; editor selection is one client) */                                         \
    X(HIGHLIGHT_MASK_VERT, highlight_highlight_mask_vert)                                                              \
    X(HIGHLIGHT_MASK_VERT_PAGED, highlight_highlight_mask_vert_paged)                                                  \
    X(HIGHLIGHT_MASK_FRAG, highlight_highlight_mask_frag)                                                              \
    X(HIGHLIGHT_BLUR_FRAG, highlight_highlight_blur_frag)                                                              \
    X(HIGHLIGHT_COMPOSITE_FRAG, highlight_highlight_composite_frag)                                                    \
    /* Streaming feedback overlay */                                                                                   \
    X(STREAMING_FEEDBACK_MASK_FRAG, streaming_feedback_streaming_mask_frag)                                            \
    X(STREAMING_FEEDBACK_COMPOSITE_FRAG, streaming_feedback_streaming_composite_frag)                                  \
    /* GPU pre-skinning compute kernel */                                                                              \
    X(SKIN_COMPUTE_COMP, skinning_skin_compute_comp)                                                                   \
    /* end */

    /// Keys for built-in feature shaders — enum values are derived from the
    /// shader list above so the two stay in sync by construction.
    enum class EBuiltinShader : uint8_t
    {
#define LUX_BUILTIN_X(name, prefix) name,
        LUX_BUILTIN_SHADER_LIST(LUX_BUILTIN_X)
#undef LUX_BUILTIN_X
            COUNT_
    };

} // namespace lux::render
