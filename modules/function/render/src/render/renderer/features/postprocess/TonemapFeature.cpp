#include <lux/engine/render/renderer/features/postprocess/TonemapFeature.hpp>
#include <lux/engine/render/renderer/features/deffer/DeferredLightingFeature.hpp>  // HDRColorTag
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/core/VulkanCheck.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <array>
#include <vector>

namespace lux::render
{

// =========================================================================
//  Construction / destruction
// =========================================================================

TonemapFeature::TonemapFeature(Config cfg)
    : cfg_(std::move(cfg))
{
    // Seed the live param struct from the bring-up Config so runtime tuning and
    // the initial render agree. cfg_ keeps shaders/targets; params_ owns the
    // tunable trio (exposure/gamma/operator).
    params_.exposure    = cfg_.exposure;
    params_.gamma       = cfg_.gamma;
    params_.tone_map_op = static_cast<std::uint32_t>(cfg_.tone_map_op);
}

TonemapFeature::~TonemapFeature()
{
    destroy();
}

// =========================================================================
//  Lifecycle
// =========================================================================

void TonemapFeature::initAndAttachTo(RenderScene& /*scene*/)
{
    init();
}

void TonemapFeature::onDetachFromScene(RenderScene& /*scene*/)
{
    // hdr_sampler_ (FifOwned<VkSampler>) retires itself through the FIF
    // deferred-destroy queue when the feature is destroyed, so the runtime
    // removeFeature UAF (#17) is closed by construction — no manual retire here.
    // §2.2: layout is owned by DescriptorService.
    hdr_ds_layout_ = VK_NULL_HANDLE;
}

// =========================================================================
//  Initialisation
// =========================================================================

void TonemapFeature::init()
{
    auto& ctx = renderContext();
    VkDevice device = ctx.deviceContext().logicalDevice();
    auto* shaders = ctx.globalRegistry().find<ShaderResources>();

    // ---- Ensure builtin shader defaults ----
    cfg_.vertex_shader   = ensureBuiltinShader(shaders, cfg_.vertex_shader,   EBuiltinShader::TONEMAP_VERT);
    cfg_.fragment_shader = ensureBuiltinShader(shaders, cfg_.fragment_shader, EBuiltinShader::TONEMAP_FRAG);

    auto& vs = *shaders->get(cfg_.vertex_shader);
    auto& fs = *shaders->get(cfg_.fragment_shader);

    // ---- HDR input descriptor set layout (1× combined_image_sampler) ----
    {
        const VkDescriptorSetLayoutBinding binding{
            0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

        auto id = ctx.descriptorService().registerLayout({.bindings = {&binding, 1},
                                                          .debug_name = "TonemapHDRDSLayout"});
        hdr_ds_layout_ = ctx.descriptorService().layout(id);
    }

    // ---- Linear-clamp sampler for HDR texture ----
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter    = VK_FILTER_LINEAR;
        si.minFilter    = VK_FILTER_LINEAR;
        si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod       = 0.0f;
        VkSampler sampler = VK_NULL_HANDLE;
        VK_CHECK(vkCreateSampler(device, &si, nullptr, &sampler));
        hdr_sampler_ = FifOwned<VkSampler>{&ctx.deferredDestroyQueue(), sampler};
    }

    // ---- Fullscreen tonemap pipeline ----
    //
    // Non-standard DS layout: Set 0 = Scene, Set 1 = HDR sampler
    {
        GraphicsPipelineTemplate tmpl{};
        tmpl.geometry_type         = EGeometryType::MESH;
        tmpl.vertex_shader         = vs.module;
        tmpl.fragment_shader       = fs.module;
        tmpl.descriptor_set_count  = 2;

        // No vertex input
        tmpl.vertex_bindings.clear();
        tmpl.vertex_attributes.clear();

        // No depth
        tmpl.depth_test_enable  = VK_FALSE;
        tmpl.depth_write_enable = VK_FALSE;

        // No blending
        tmpl.blend_enable = VK_FALSE;

        // No culling (fullscreen triangle)
        tmpl.cull_mode = VK_CULL_MODE_NONE;

        // Non-standard resource_slot_map: Scene → slot 0 only
        tmpl.resource_slot_map.push_back({EDescriptorSetSlot::Scene, 0});

        // Push constants: offset 0 = shared (scene_index, view_index, 8 bytes)
        //                 offset 8 = tonemap params (exposure, gamma, operator_id, 12 bytes)
        const VkPushConstantRange pc{
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20};
        const std::array set_layouts{
            ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Scene),  // set 0
            hdr_ds_layout_,                                               // set 1
        };
        const std::array pcs{pc};
        tmpl.pipeline_layout = ctx.pipelineLayoutService().getOrCreate({
            .set_layouts    = set_layouts,
            .push_constants = pcs,
            .debug_name     = "TonemapLayout"
        }).value();
        // Authoritative PC ranges from the layout — prevents reflection
        // from dropping VERTEX_BIT (fullscreen vertex shader has no PC block).
        tmpl.push_constant_ranges.assign(pcs.begin(), pcs.end());

        std::vector<const rdesc::ShaderInfo*> infos = {
            &vs.info, &fs.info};
        tonemap_pipeline_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos).value();
    }
}

void TonemapFeature::destroy() noexcept
{
    // hdr_sampler_ (FifOwned) retires through the deferred-destroy queue when this
    // feature is destroyed — no inline vkDestroySampler. (C1)
    // §2.2: Layout owned by DescriptorService — no manual destroy.
    hdr_ds_layout_ = VK_NULL_HANDLE;
}

// =========================================================================
//  Per-frame: apply staged params (mirrors GridPassFeature's pending pattern,
//  consumed from RenderScene::beginFrame's onFrameBegin loop).
// =========================================================================

void TonemapFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
{
    if (pending_params_)
    {
        params_ = *pending_params_;
        pending_params_.reset();
    }
}

// =========================================================================
//  Render graph passes
// =========================================================================

void TonemapFeature::addPasses(RGBuilder& builder)
{
    // ---- Reference lit color target (forward ref resolved at compile time) ----
    // If DeferredLightingFeature is absent, this pass is pruned by dead-pass elimination.
    auto lit_color = builder.referenceTexture(cfg_.color_input);

    // ---- Transient lit-color descriptor set (per-view per-frame) ----
    auto lit_tds = builder.createTransientDS("LitColorDS", hdr_ds_layout_, {
        {0, EDescriptorType::COMBINED_IMAGE_SAMPLER, lit_color, hdr_sampler_.get(),
         EImageLayout::SHADER_READ_ONLY_OPTIMAL},
    });

    builder.addPass("Tonemap", ERGPassType::GRAPHICS)
        .read(lit_color, lux::common::ETextureRole::SAMPLED)
        .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
        .setPipeline(tonemap_pipeline_)
        .bindSceneDS(0)             // Set 0: Scene
        .bindTransientDS(1, lit_tds)  // Set 1: Lit color sampler (per-view transient)
        .setKernelFn([this](const PassRecordContext& rec) {
            // Push tonemap parameters (offset 8, after shared scene_index/view_index)
            struct TonemapPC {
                float    exposure;
                float    gamma;
                uint32_t operator_id;
            } pc{params_.exposure, params_.gamma, params_.tone_map_op};
            vkCmdPushConstants(rec.cmd, rec.pipeline_layout,
                rec.pc_stage_flags, 8, sizeof(pc), &pc);

            // Draw fullscreen triangle
            vkCmdDraw(rec.cmd, 3, 1, 0, 0);
        })
        .setKernel("TonemapPass")
        .stage(ERenderStage::PostProcess);   // HDR→LDR scene composite — runs BEFORE the
                                             // Overlay-stage grid/gizmos that draw on top
}

} // namespace lux::render
