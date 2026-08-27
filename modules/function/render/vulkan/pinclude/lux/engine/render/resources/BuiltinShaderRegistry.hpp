#pragma once
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/function/render/client/core/ResourceHandle.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/function/render/client/resources/EBuiltinShader.hpp>
// EBuiltinShader enum + LUX_BUILTIN_SHADER_LIST X-macro (public)
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
#include "grid_grid2d_frag_embed.hpp"
// ── Trajectory ──
#include "trajectory_trajectory_line_vert_embed.hpp"
#include "trajectory_trajectory_line_frag_embed.hpp"
// ── Deferred GBuffer ──
// Collapsed to a single mesh vertex shader (the _vp variant reads
// vertices through the bindless pool — set 7 — for static and skinned alike).
// The legacy attribute-path gbuffer.vert was deleted.
#include "deferred_gbuffer_vp_vert_embed.hpp"
#include "deferred_gbuffer_vp_vert_paged_embed.hpp"
#include "deferred_gbuffer_unlit_frag_embed.hpp"
#include "deferred_gbuffer_pbr_frag_embed.hpp"
#include "deferred_gbuffer_stylized_frag_embed.hpp"
// ── Deferred Lighting ──
#include "deferred_deferred_lighting_vert_embed.hpp"
// .frag has one SPIR-V per shadow technique (see EVSM plan §3 / C1.3).
#include "deferred_deferred_lighting_frag_pcf_embed.hpp"
#include "deferred_deferred_lighting_frag_evsm_embed.hpp"
#include "deferred_deferred_lighting_frag_pcf_lr_embed.hpp"
#include "deferred_deferred_lighting_frag_evsm_lr_embed.hpp"
#include "deferred_cluster_build_comp_embed.hpp"
#include "deferred_cluster_count_comp_embed.hpp"
#include "deferred_cluster_scan_comp_embed.hpp"
#include "deferred_cluster_fill_comp_embed.hpp"
// ── Forward / shared compute ──
// mesh_cull_unified has HZB on/off build variants (no_hzb / hzb) — the no_hzb
// SPIR-V never declares descriptor set 1, so set-0-only cull pipelines stay valid.
#include "forward_mesh_cull_unified_comp_no_hzb_embed.hpp"
#include "forward_mesh_cull_unified_comp_hzb_embed.hpp"
#include "forward_mesh_cull_unified_comp_no_hzb_paged_embed.hpp"
#include "forward_mesh_cull_unified_comp_hzb_paged_embed.hpp"
#include "forward_mdc_compact_comp_embed.hpp"
#include "forward_clear_count_buffers_comp_embed.hpp"
// ── HZB occlusion pyramid (P2 Stage A) ──
#include "hzb_downsample_comp_embed.hpp"
#include "render_cluster_cluster_cull_comp_embed.hpp"
#include "render_cluster_candidate_expand_comp_embed.hpp"
#include "render_cluster_pick_vert_embed.hpp"
#include "render_cluster_pick_frag_embed.hpp"
// ── Terrain GPU patch selection and GBuffer raster ──
#include "terrain_patch_select_comp_embed.hpp"
#include "terrain_patch_vert_embed.hpp"
#include "terrain_patch_frag_embed.hpp"
// Collapsed to single _vp mesh vertex shader (attribute-path
// forward_mesh.vert deleted).
#include "forward_forward_mesh_vp_vert_embed.hpp"
#include "forward_forward_mesh_vp_vert_paged_embed.hpp"
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
#include "shadow_mesh_shadow_vp_vert_paged_embed.hpp"
#include "shadow_shadow_depth_vp_vert_embed.hpp"
#include "shadow_shadow_depth_vp_vert_paged_embed.hpp"
#include "shadow_shadow_depth_frag_embed.hpp"
// EVSM technique (C3): moment caster + separable Gaussian blur.
#include "shadow_shadow_evsm_caster_frag_embed.hpp"
#include "shadow_shadow_evsm_blur_h_comp_embed.hpp"
#include "shadow_shadow_evsm_blur_v_comp_embed.hpp"
#include "skinning_skin_compute_comp_embed.hpp" // GPU pre-skinning kernel
// ── Skybox ──
#include "skybox_skybox_vert_embed.hpp"
#include "skybox_skybox_cubemap_frag_embed.hpp"
#include "skybox_skybox_equirect_frag_embed.hpp"
// ── Tonemap ──
#include "postprocess_tonemap_vert_embed.hpp"
#include "postprocess_tonemap_frag_embed.hpp"
#include "postprocess_linear_depth_frag_embed.hpp"
#include "postprocess_ssao_frag_embed.hpp"
#include "postprocess_fog_frag_embed.hpp"
#include "water_water_frag_embed.hpp"
// ── Gizmo ──
#include "gizmo_line_list_vert_embed.hpp"
#include "gizmo_line_list_frag_embed.hpp"
#include "gizmo_tri_overlay_vert_embed.hpp"
// gizmo_tri_overlay_frag was byte-identical to gizmo_line_list_frag; TRI_OVERLAY_FRAG
// now aliases the line_list frag blob (single source), so no embed header for it. (R-3)
// ── Canvas2D ──
#include "canvas2d_image_vert_embed.hpp"
#include "canvas2d_image_frag_embed.hpp"
#include "canvas2d_pixel_field_vert_embed.hpp"
#include "canvas2d_pixel_field_frag_embed.hpp"
#include "canvas2d_tile_vert_embed.hpp"
#include "canvas2d_tile_frag_embed.hpp"
#include "canvas2d_group_composite_frag_embed.hpp"
// ── Highlight outline ──
#include "highlight_highlight_mask_vert_embed.hpp"
#include "highlight_highlight_mask_vert_paged_embed.hpp"
#include "highlight_highlight_mask_frag_embed.hpp"
#include "highlight_highlight_blur_frag_embed.hpp"
#include "highlight_highlight_composite_frag_embed.hpp"
// ── Asset streaming feedback ──
#include "streaming_feedback_streaming_mask_frag_embed.hpp"
#include "streaming_feedback_streaming_composite_frag_embed.hpp"

namespace lux::render
{
    [[nodiscard]] inline EBuiltinShader instanceStorageVariant(EBuiltinShader shader, bool sparse_pages) noexcept
    {
        if (!sparse_pages)
            return shader;
        switch (shader)
        {
        case EBuiltinShader::GBUFFER_VERT:
            return EBuiltinShader::GBUFFER_VERT_PAGED;
        case EBuiltinShader::MESH_CULL_UNIFIED_COMP:
            return EBuiltinShader::MESH_CULL_UNIFIED_COMP_PAGED;
        case EBuiltinShader::MESH_CULL_UNIFIED_COMP_HZB:
            return EBuiltinShader::MESH_CULL_UNIFIED_COMP_HZB_PAGED;
        case EBuiltinShader::FORWARD_MESH_VERT:
            return EBuiltinShader::FORWARD_MESH_VERT_PAGED;
        case EBuiltinShader::MESH_SHADOW_VERT:
            return EBuiltinShader::MESH_SHADOW_VERT_PAGED;
        case EBuiltinShader::SHADOW_DEPTH_VERT:
            return EBuiltinShader::SHADOW_DEPTH_VERT_PAGED;
        case EBuiltinShader::HIGHLIGHT_MASK_VERT:
            return EBuiltinShader::HIGHLIGHT_MASK_VERT_PAGED;
        default:
            return shader;
        }
    }

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
#define LUX_BUILTIN_X(name, prefix)                                                                                    \
    case EBuiltinShader::name:                                                                                         \
        return {as_bytes(prefix##_spirv, prefix##_spirv_size), as_bytes(prefix##_info, prefix##_info_size)};
            LUX_BUILTIN_SHADER_LIST(LUX_BUILTIN_X)
#undef LUX_BUILTIN_X
        default:
            return {{}, {}};
        }
    }

    /// 解析一个 stage 的着色器句柄:@p configured 有效则原样返回,否则把内置着色器
    /// 注册进 @p shaders。
    ///
    /// 解析不出模块时返回 shader.builtin_unavailable 并带上是**哪一个** —— 而不是像
    /// 从前那样静默交回无效句柄。那条静默路径是一整串崩溃的起点:无效句柄一路传到
    /// `*shaders.get(h)` 才炸,现场离病根很远。
    [[nodiscard]] inline Expected<ShaderHandle>
    resolveShaderStage(ShaderResources& shaders, ShaderHandle configured, EBuiltinShader builtin)
    {
        if (configured.isValid())
            return configured;

        builtin = instanceStorageVariant(builtin, shaders.sparseInstancePages());
        const auto builtin_arg = static_cast<std::uint32_t>(builtin);

        const BuiltinShaderData data = getBuiltinShader(builtin);
        if (data.spirv.empty())
            return renderFailure<err::shader::BuiltinUnavailable>(builtin_arg);

        lux::rdesc::ShaderInfo info;
        if (!lux::rdesc::ShaderInfo::deserialize(data.info, info))
            return renderFailure<err::shader::BuiltinUnavailable>(builtin_arg);

        const ShaderHandle handle = shaders.add(data.spirv, info);
        if (handle.isNull())
            return renderFailure<err::device::VulkanObjectCreationFailed>();
        return handle;
    }

    /// 一个待回填的着色器句柄槽:目标句柄有效则原样保留,无效则用内置解析结果填上。
    struct ShaderStageSlot
    {
        EBuiltinShader builtin{};
        ShaderHandle* target{nullptr};
    };

    /// 批量回填一组句柄槽(典型场景:feature 的 Config 里有一串 shader 字段,客户端
    /// 只覆盖了其中几个)。任一槽解析失败即整体报错,不会留下半填状态。
    [[nodiscard]] inline Expected<void>
    resolveShaderStages(ShaderResources& shaders, std::span<const ShaderStageSlot> slots)
    {
        for (const ShaderStageSlot& slot : slots)
        {
            if (slot.target == nullptr)
                continue;
            auto handle = resolveShaderStage(shaders, *slot.target, slot.builtin);
            if (!handle)
                return lux::cxx::unexpected(handle.error());
            *slot.target = *handle;
        }
        return {};
    }

    /// 一个 stage 的请求:内置着色器 id,加上调用方可能已经显式配置的覆盖句柄。
    struct PipelineStageRequest
    {
        EBuiltinShader builtin{};  ///< configured 无效时用它解析
        ShaderHandle configured{}; ///< 调用方给的覆盖句柄;有效则原样使用
    };

    /// 解析并切换一条管线的全部 stage —— feature 侧一次调用拿到全部模块与反射。
    ///
    /// 它把「解析内置着色器」和「批量域合并切换」串成一步,于是 feature 里再也不需要
    /// 逐 stage 写 ensure → merge → get 三段,也就不再有写错顺序的余地。
    [[nodiscard]] inline Expected<PreparedPipelineStages>
    preparePipelineStages(ShaderResources& shaders, std::span<const PipelineStageRequest> requests)
    {
        std::vector<ShaderHandle> resolved;
        resolved.reserve(requests.size());

        for (const PipelineStageRequest& request : requests)
        {
            auto handle = resolveShaderStage(shaders, request.configured, request.builtin);
            if (!handle)
                return lux::cxx::unexpected(handle.error());
            resolved.push_back(*handle);
        }
        return shaders.preparePipelineStages(resolved);
    }

} // namespace lux::render
