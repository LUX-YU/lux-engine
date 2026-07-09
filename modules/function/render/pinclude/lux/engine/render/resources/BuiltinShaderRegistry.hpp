#pragma once
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/EBuiltinShader.hpp> // EBuiltinShader enum + LUX_BUILTIN_SHADER_LIST X-macro (public)
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <optional>

// Include all generated embed headers.
// These are produced at build time by lux_asset_packer --embed.
// ── Point cloud ──
#include "point_cloud_pointcloud_simple_vert_embed.hpp"
#include "point_cloud_pointcloud_simple_frag_embed.hpp"
#include "point_cloud_pointcloud_lod_vert_embed.hpp"
#include "point_cloud_pointcloud_splat_frag_embed.hpp"
#include "point_cloud_pointcloud_culling_comp_embed.hpp"
// ── Grid ──
#include "grid_grid_vert_embed.hpp"
#include "grid_grid_frag_embed.hpp"
// ── Trajectory ──
#include "trajectory_trajectory_line_vert_embed.hpp"
#include "trajectory_trajectory_line_frag_embed.hpp"
// ── Deferred GBuffer ──
// Collapsed to a single mesh vertex shader (the _vp variant reads
// vertices through the bindless pool — set 7 — for static and skinned alike).
// The legacy attribute-path gbuffer.vert was deleted.
#include "deferred_gbuffer_vp_vert_embed.hpp"
#include "deferred_gbuffer_unlit_frag_embed.hpp"
#include "deferred_gbuffer_pbr_frag_embed.hpp"
#include "deferred_gbuffer_stylized_frag_embed.hpp"
// ── Deferred Lighting ──
#include "deferred_deferred_lighting_vert_embed.hpp"
// .frag has one SPIR-V per shadow technique (see EVSM plan §3 / C1.3).
#include "deferred_deferred_lighting_frag_pcf_embed.hpp"
#include "deferred_deferred_lighting_frag_evsm_embed.hpp"
#include "deferred_cluster_build_comp_embed.hpp"
#include "deferred_cluster_count_comp_embed.hpp"
#include "deferred_cluster_scan_comp_embed.hpp"
#include "deferred_cluster_fill_comp_embed.hpp"
// ── Forward / shared compute ──
// mesh_cull_unified has HZB on/off build variants (no_hzb / hzb) — the no_hzb
// SPIR-V never declares descriptor set 1, so set-0-only cull pipelines stay valid.
#include "forward_mesh_cull_unified_comp_no_hzb_embed.hpp"
#include "forward_mesh_cull_unified_comp_hzb_embed.hpp"
#include "forward_mdc_compact_comp_embed.hpp"
#include "forward_clear_count_buffers_comp_embed.hpp"
// ── HZB occlusion pyramid (P2 Stage A) ──
#include "hzb_downsample_comp_embed.hpp"
// Collapsed to single _vp mesh vertex shader (attribute-path
// forward_mesh.vert deleted).
#include "forward_forward_mesh_vp_vert_embed.hpp"
#include "forward_fr_unlit_frag_embed.hpp"
// PBR / stylized .frag has one SPIR-V per shadow technique (EVSM plan §3 / C1.3).
#include "forward_fr_pbr_frag_pcf_embed.hpp"
#include "forward_fr_pbr_frag_evsm_embed.hpp"
#include "forward_fr_stylized_frag_pcf_embed.hpp"
#include "forward_fr_stylized_frag_evsm_embed.hpp"
// ── Shadow ──
// Both verts use the same bindless pool / instance / visible descriptor sets
// — they differ only in the output interpolant set.
//   mesh_shadow_vp.vert: fat vert (vUV + vShadowNear/Far/DepthPersp), paired
//                        with shadow_evsm_caster.frag.
//   shadow_depth_vp.vert: thin vUV-only vert, paired with shadow_depth.frag
//                         (PCF / depth-only). Splitting them silences the
//                         SPIR-V interface validator's OutputNotConsumed
//                         warning at locations 1/2/3.
#include "shadow_mesh_shadow_vp_vert_embed.hpp"
#include "shadow_shadow_depth_vp_vert_embed.hpp"
#include "shadow_shadow_depth_frag_embed.hpp"
// EVSM technique (C3): moment caster + separable Gaussian blur.
#include "shadow_shadow_evsm_caster_frag_embed.hpp"
#include "shadow_shadow_evsm_blur_h_comp_embed.hpp"
#include "shadow_shadow_evsm_blur_v_comp_embed.hpp"
#include "skinning_skin_compute_comp_embed.hpp"      // GPU pre-skinning kernel
// ── Skybox ──
#include "skybox_skybox_vert_embed.hpp"
#include "skybox_skybox_cubemap_frag_embed.hpp"
#include "skybox_skybox_equirect_frag_embed.hpp"
// ── Tonemap ──
#include "postprocess_tonemap_vert_embed.hpp"
#include "postprocess_tonemap_frag_embed.hpp"
// ── Gizmo ──
#include "gizmo_line_list_vert_embed.hpp"
#include "gizmo_line_list_frag_embed.hpp"
#include "gizmo_tri_overlay_vert_embed.hpp"
#include "gizmo_tri_overlay_frag_embed.hpp"
// ── Canvas2D ──
#include "canvas2d_sprite_vert_embed.hpp"
#include "canvas2d_sprite_frag_embed.hpp"
#include "canvas2d_pixel_field_vert_embed.hpp"
#include "canvas2d_pixel_field_frag_embed.hpp"
// ── Highlight outline ──
#include "highlight_highlight_mask_vert_embed.hpp"
#include "highlight_highlight_mask_frag_embed.hpp"
#include "highlight_highlight_blur_frag_embed.hpp"
#include "highlight_highlight_composite_frag_embed.hpp"

namespace lux::render
{
    /// A reference to an embedded shader's raw data (SPIR-V + serialized ShaderInfo).
    struct BuiltinShaderData
    {
        std::span<const std::byte> spirv;
        std::span<const std::byte> info;
    };


    /// Returns the embedded data for a built-in shader.
    inline BuiltinShaderData getBuiltinShader(EBuiltinShader key)
    {
        using namespace builtin;
        auto as_bytes = [](const uint8_t* p, std::size_t n) {
            return std::span<const std::byte>{reinterpret_cast<const std::byte*>(p), n};
        };

        switch (key)
        {
#define LUX_BUILTIN_X(name, prefix)                                                  \
        case EBuiltinShader::name:                                                   \
            return {as_bytes(prefix##_spirv, prefix##_spirv_size),                   \
                    as_bytes(prefix##_info,  prefix##_info_size)};
            LUX_BUILTIN_SHADER_LIST(LUX_BUILTIN_X)
#undef LUX_BUILTIN_X
        default:
            return {{}, {}};
        }
    }

    /// If \p configured is null, loads the built-in shader into \p shaders and
    /// returns the new handle.  Otherwise returns \p configured unchanged.
    inline ShaderHandle ensureBuiltinShader(
        ShaderResources* shaders,
        ShaderHandle     configured,
        EBuiltinShader   builtin)
    {
        if (configured.valid())
            return configured;

        auto data = getBuiltinShader(builtin);
        if (data.spirv.empty())
            return configured; // unknown builtin — leave invalid

        lux::rdesc::ShaderInfo info;
        if (!lux::rdesc::ShaderInfo::deserialize(data.info, info))
            return configured; // deserialization failed

        return shaders->add(data.spirv, info);
    }

} // namespace lux::render
