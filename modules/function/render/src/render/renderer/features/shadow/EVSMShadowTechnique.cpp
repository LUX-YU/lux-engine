/**
 * @file EVSMShadowTechnique.cpp
 * @brief EVSM technique's self-owned blur pipelines + post-frame blur passes.
 *
 * Moved here (out of ShadowMapFeature / MeshShadowFeature) so the EVSM-specific
 * blur compute pipelines and the two separable blur passes live entirely inside
 * the technique object. MeshShadowFeature drives them polymorphically via
 * IShadowTechnique::recordPostFrame — it no longer knows EVSM exists. Adding a
 * new technique = a new IShadowTechnique subclass; zero change here.
 */

#include <lux/engine/render/renderer/features/shadow/EVSMShadowTechnique.hpp>

#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>   // ensureBuiltinShader
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapTypes.hpp>   // ShadowSliceGPU
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace lux::render
{
    void EVSMShadowTechnique::ensureBlurPipelines(RenderContext& ctx)
    {
        auto* shaders = ctx.globalRegistry().find<ShaderResources>();
        if (!shaders) return;

        // Lazily resolve the blur compute shader handles from the builtin registry.
        ShaderHandle blur_h_handle{};
        ShaderHandle blur_v_handle{};
        blur_h_handle = ensureBuiltinShader(shaders, blur_h_handle, EBuiltinShader::SHADOW_EVSM_BLUR_H_COMP);
        blur_v_handle = ensureBuiltinShader(shaders, blur_v_handle, EBuiltinShader::SHADOW_EVSM_BLUR_V_COMP);

        auto* blur_h_obj = shaders->get(blur_h_handle);
        auto* blur_v_obj = shaders->get(blur_v_handle);
        if (!blur_h_obj || !blur_v_obj) return;

        // ── Descriptor set layout shared by both blur passes ──────────────────
        // Binding 0: SAMPLED_IMAGE (texelFetch, no sampler). Binding 1: STORAGE_IMAGE.
        if (blur_ds_layout_ == VK_NULL_HANDLE)
        {
            std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
            bindings[0].binding         = 0;
            bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[1].binding         = 1;
            bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

            auto id = ctx.descriptorService().registerLayout({
                .bindings = bindings,
                .flags = 0,
                .debug_name = "EVSMBlurDSLayout"});
            blur_ds_layout_ = ctx.descriptorService().layout(id);
        }

        // Push constant range matches shadow_evsm_blur_{h,v}.comp's BlurPC.
        const VkPushConstantRange blur_pc{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset     = 0,
            .size       = sizeof(int32_t) * 6   // ivec2 + ivec2 + uint + uint
        };
        const std::array layouts{blur_ds_layout_};
        const std::array pcs{blur_pc};

        auto pl = ctx.pipelineLayoutService().getOrCreate({
            .set_layouts    = layouts,
            .push_constants = pcs,
            .debug_name     = "EVSMBlurLayout"});
        if (!pl.has_value()) return;

        if (!blur_h_pipeline_.valid())
            blur_h_pipeline_ = ctx.pipelineManager().registerComputePipeline(
                blur_h_obj->module, pl.value());
        if (!blur_v_pipeline_.valid())
            blur_v_pipeline_ = ctx.pipelineManager().registerComputePipeline(
                blur_v_obj->module, pl.value());
    }

    void EVSMShadowTechnique::recordPostFrame(const ShadowFrameContext& ctx)
    {
        if (!blur_h_pipeline_.valid() || !blur_v_pipeline_.valid())
            return;
        if (ctx.builder == nullptr || ctx.shadow_res == nullptr || ctx.scene == nullptr)
            return;

        RGBuilder&            builder    = *ctx.builder;
        const uint32_t        page_res   = ctx.atlas_resolution;
        const VkDescriptorSetLayout blur_ds_layout = blur_ds_layout_;
        const ComputePipelineHandle blur_h = blur_h_pipeline_;
        const ComputePipelineHandle blur_v = blur_v_pipeline_;

        // Push constant layout (matches shadow_evsm_blur_{h,v}.comp BlurPC).
        struct BlurPC { int32_t tile_min[2]; int32_t tile_max[2]; uint32_t atlas_layer; uint32_t _pad; };

        // Per-slice tile-bounded blur — keeps neighbouring tiles in the same atlas
        // page from cross-contaminating (the "Cthulhu" per-tile bleeding seen during
        // the 2026-06-05 EVSM bring-up). Captures by value (the lambda runs at record
        // time, after this function returns — must not capture the ctx reference).
        ShadowResources* shadow_res_ptr = ctx.shadow_res;
        RenderScene*     scene_ptr      = ctx.scene;

        auto blur_kernel = [page_res, shadow_res_ptr, scene_ptr](const PassRecordContext& pctx) {
            if (pctx.pipeline_layout == VK_NULL_HANDLE) return;
            const uint32_t view_handle = (pctx.view != nullptr) ? pctx.view->handle : 0u;
            const uint32_t scene_key   = scene_ptr->sceneGlobalSlot().index;
            // Pin the slice list for the whole loop: the frame thread may swap the
            // cache entry concurrently; the shared_ptr keeps our slices alive.
            auto slice_cache = shadow_res_ptr->findViewCache(scene_key, view_handle);
            const std::span<const ShadowSliceGPU> slices = slice_cache
                ? std::span<const ShadowSliceGPU>{ slice_cache->slices }
                : std::span<const ShadowSliceGPU>{};
            if (slices.empty())
                return;

            const float fres = static_cast<float>(page_res);
            for (const auto& s : slices)
            {
                const int32_t tx0 = static_cast<int32_t>(std::floor(s.atlas_uv_bias.x() * fres));
                const int32_t ty0 = static_cast<int32_t>(std::floor(s.atlas_uv_bias.y() * fres));
                const int32_t tx1 = static_cast<int32_t>(std::ceil((s.atlas_uv_bias.x() + s.atlas_uv_scale.x()) * fres)) - 1;
                const int32_t ty1 = static_cast<int32_t>(std::ceil((s.atlas_uv_bias.y() + s.atlas_uv_scale.y()) * fres)) - 1;
                if (tx1 < tx0 || ty1 < ty0) continue;
                const uint32_t gw = (static_cast<uint32_t>(tx1 - tx0 + 1) + 7u) / 8u;
                const uint32_t gh = (static_cast<uint32_t>(ty1 - ty0 + 1) + 7u) / 8u;
                BlurPC pc{ {tx0, ty0}, {tx1, ty1}, s.atlas_layer, 0u };
                vkCmdPushConstants(pctx.cmd, pctx.pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlurPC), &pc);
                vkCmdDispatch(pctx.cmd, gw, gh, 1u);
            }
        };

        // Horizontal blur: moment → scratch.
        auto blur_h_tds = builder.createTransientDS(
            "EVSMBlurHDS", blur_ds_layout,
            {
                {0, EDescriptorType::SAMPLED_IMAGE,
                 builder.referenceTexture("evsm_moment_atlas"),
                 VK_NULL_HANDLE, EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                {1, EDescriptorType::STORAGE_IMAGE,
                 builder.referenceTexture("evsm_scratch_atlas"),
                 VK_NULL_HANDLE, EImageLayout::GENERAL},
            });
        builder.addPass("EVSMBlurH", ERGPassType::COMPUTE)
            .setComputePipeline(blur_h)
            .bindTransientDS(0, blur_h_tds)
            .read(builder.referenceTexture("evsm_moment_atlas"),  lux::common::ETextureRole::SAMPLED)
            .write(builder.referenceTexture("evsm_scratch_atlas"), lux::common::ETextureRole::UNORDERED_ACCESS)
            .after("MeshShadowDraw")
            .setKernelFn(blur_kernel);

        // Vertical blur: scratch → moment (reused as the final blurred atlas).
        auto blur_v_tds = builder.createTransientDS(
            "EVSMBlurVDS", blur_ds_layout,
            {
                {0, EDescriptorType::SAMPLED_IMAGE,
                 builder.referenceTexture("evsm_scratch_atlas"),
                 VK_NULL_HANDLE, EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                {1, EDescriptorType::STORAGE_IMAGE,
                 builder.referenceTexture("evsm_moment_atlas"),
                 VK_NULL_HANDLE, EImageLayout::GENERAL},
            });
        builder.addPass("EVSMBlurV", ERGPassType::COMPUTE)
            .setComputePipeline(blur_v)
            .bindTransientDS(0, blur_v_tds)
            .read(builder.referenceTexture("evsm_scratch_atlas"),  lux::common::ETextureRole::SAMPLED)
            .write(builder.referenceTexture("evsm_moment_atlas"), lux::common::ETextureRole::UNORDERED_ACCESS)
            .after("EVSMBlurH")
            .setKernelFn(blur_kernel);
    }

} // namespace lux::render
