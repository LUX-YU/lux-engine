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

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>   // resolveShaderStage
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <lux/engine/function/render/client/resources/lighting/ShadowMapTypes.hpp>   // ShadowSliceGPU
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/function/render/client/features/shadow/MeshShadowOperation.hpp>            // kMeshShadowDrawPassName
#include <lux/engine/function/render/client/features/shadow/ShadowMapOperation.hpp>             // kEvsmBlurVPassName
#include <lux/engine/render/scene/View.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace lux::render
{
    void EVSMShadowTechnique::ensureBlurPipelines(RenderContext& ctx)
    {
        auto& shaders = ctx.globalRegistry().must<ShaderResources>();

        // 惰性解析两条模糊 compute 着色器。
        const auto blur_h_handle =
            resolveShaderStage(shaders, ShaderHandle{}, EBuiltinShader::SHADOW_EVSM_BLUR_H_COMP);
        const auto blur_v_handle =
            resolveShaderStage(shaders, ShaderHandle{}, EBuiltinShader::SHADOW_EVSM_BLUR_V_COMP);
        if (!blur_h_handle || !blur_v_handle)
            return;

        const ShaderObject* blur_h_obj = shaders.get(*blur_h_handle);
        const ShaderObject* blur_v_obj = shaders.get(*blur_v_handle);
        if (!blur_h_obj || !blur_v_obj)
            return;

        // Both blur pipelines' layouts are built via reflection (set0 = {SAMPLED_IMAGE
        // input, STORAGE_IMAGE output}; no pass-local contract resources; push
        // constants are also derived from reflection). Since both shaders have the
        // same set shape, PipelineLayoutService/DescriptorService dedupe them down
        // to the same layout object, matching hand-built behavior.
        if (!blur_h_pipeline_.valid())
        {
            auto h = ctx.pipelineManager().registerComputePipelineReflected(
                blur_h_obj->module, blur_h_obj->info, "EVSMBlurH");
            if (!h) return;
            blur_h_pipeline_ = *h;
        }
        if (!blur_v_pipeline_.valid())
        {
            auto v = ctx.pipelineManager().registerComputePipelineReflected(
                blur_v_obj->module, blur_v_obj->info, "EVSMBlurV");
            if (!v) return;
            blur_v_pipeline_ = *v;
        }
        // Both pipelines' set0 is the same layout (identical shape means the service
        // dedupes them), so it's fine to just take it from H. This line sits outside
        // both branches above so it stays valid even under partial construction
        // (e.g. H already existed, V was just created).
        blur_ds_layout_ = ctx.pipelineManager().computeSetLayout(blur_h_pipeline_, 0);
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
            const uint32_t view_handle = (pctx.view != nullptr) ? pctx.view->handle.index : 0u;
            const uint32_t scene_key   = scene_ptr->sceneGlobalSlot().index;
            // Pin the slice list for the whole loop: a later setCachedData (same
            // thread — this kernel is REPLAYED by the cached graph) swaps the
            // cache entry; the shared_ptr keeps our slices alive.
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
            .after(kMeshShadowDrawPassName)
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
        builder.addPass(kEvsmBlurVPassName, ERGPassType::COMPUTE)
            .setComputePipeline(blur_v)
            .bindTransientDS(0, blur_v_tds)
            .read(builder.referenceTexture("evsm_scratch_atlas"),  lux::common::ETextureRole::SAMPLED)
            .write(builder.referenceTexture("evsm_moment_atlas"), lux::common::ETextureRole::UNORDERED_ACCESS)
            .after("EVSMBlurH")
            .setKernelFn(blur_kernel);
    }

} // namespace lux::render
