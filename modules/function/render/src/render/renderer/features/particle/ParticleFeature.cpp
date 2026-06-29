#include <lux/engine/render/renderer/features/particle/ParticleFeature.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGResourceTypes.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/pipeline/StandardPipelineLayoutBuilder.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/core/Geometry.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>

#include <array>
#include <memory>
#include <vector>

namespace lux::render
{

// =============================================================================
// ParticleFeature — lifecycle implementation
// =============================================================================

ParticleFeature::~ParticleFeature()
{
    // We only need to clean up Vulkan objects created here.
    // compute_pipeline_ and its layout are now owned by PipelineManager
    // (registered via registerComputePipeline) — do NOT destroy them here.
    // ParticleResources and draw_layout_ are owned by SceneResourceRegistry and
    // GeneralDescriptorSetLayout respectively — do NOT destroy them here.
}

void ParticleFeature::onSyncSetParticleParams(const SetParticleSimParamsCmd& cmd, const FeatureFrameContext& /*ctx*/) noexcept
{
    pending_delta_time_ = cmd.delta_time;
    pending_gravity_ = cmd.gravity;
    has_pending_params_ = true;
}

void ParticleFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
{
    if (!has_pending_params_) return;
    delta_time_ = pending_delta_time_;
    gravity_    = pending_gravity_;
    has_pending_params_ = false;
}

// ---------------------------------------------------------------------------
// Constructor — pipeline registration + resource init
// ---------------------------------------------------------------------------
ParticleFeature::ParticleFeature(Config cfg)
    : cfg_(std::move(cfg))
{
}

void ParticleFeature::initAndAttachTo(RenderScene& /*scene*/)
{
    init(cfg_);
}

ParticleResources* ParticleFeature::particleResources() noexcept
{
    return renderScene().sceneRegistry().find<ParticleResources>();
}

void ParticleFeature::init(const Config& cfg)
{
    auto& ctx = renderContext();
    auto* shaders = ctx.globalRegistry().find<ShaderResources>();
    auto& cs = *shaders->get(cfg.compute_shader);
    auto& vs = *shaders->get(cfg.vertex_shader);
    auto& fs = *shaders->get(cfg.fragment_shader);
    // ------------------------------------------------------------------
    // 1. Compute pipeline layout
    // ------------------------------------------------------------------
    {
        const VkDescriptorSetLayout particle_layout = ctx.descriptorLayouts().getParticleSetLayout();
        const VkPushConstantRange pc{
            VK_SHADER_STAGE_COMPUTE_BIT, 0, 16
        };
        const std::array<VkDescriptorSetLayout, 1> set_layouts{particle_layout};
        const std::array<VkPushConstantRange, 1> pc_ranges{pc};
        compute_layout_ = ctx.pipelineLayoutService().getOrCreate({
            .set_layouts = set_layouts,
            .push_constants = pc_ranges,
            .debug_name = "ParticleComputeLayout"
        });
    }

    // ------------------------------------------------------------------
    // 2. Register compute pipeline
    // ------------------------------------------------------------------
    {
        compute_pipeline_handle_ = ctx.pipelineManager().registerComputePipeline(
            cs.module, compute_layout_);
    }

    // ------------------------------------------------------------------
    // 3. Register graphics pipeline template: billboard + additive blend
    // ------------------------------------------------------------------
    {
        GraphicsPipelineTemplate tmpl{};
        tmpl.geometry_type        = EGeometryType::MESH;
        tmpl.descriptor_set_count = 6;
        tmpl.vertex_shader        = vs.module;
        tmpl.fragment_shader      = fs.module;

        tmpl.topology           = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        tmpl.cull_mode          = VK_CULL_MODE_NONE;
        tmpl.depth_test_enable  = VK_TRUE;
        tmpl.depth_write_enable = VK_FALSE;
        tmpl.depth_compare_op   = VK_COMPARE_OP_LESS_OR_EQUAL;

        tmpl.blend_enable              = VK_TRUE;
        tmpl.src_color_blend_factor    = VK_BLEND_FACTOR_ONE;
        tmpl.dst_color_blend_factor    = VK_BLEND_FACTOR_ONE;
        tmpl.color_blend_op            = VK_BLEND_OP_ADD;
        tmpl.src_alpha_blend_factor    = VK_BLEND_FACTOR_ONE;
        tmpl.dst_alpha_blend_factor    = VK_BLEND_FACTOR_ZERO;
        tmpl.alpha_blend_op            = VK_BLEND_OP_ADD;

        std::vector<const lux::rdesc::ShaderInfo*> infos = { &vs.info, &fs.info };
        tmpl.pipeline_layout = buildStandardGraphicsPipelineLayout(
            ctx,
            tmpl.descriptor_set_count,
            infos,
            "ParticleDrawLayout").value();
        draw_pipeline_handle_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos).value();
    }

    // ------------------------------------------------------------------
    // 4. Init resources (per-scene, lazy registration)
    // ------------------------------------------------------------------
    {
        if (!renderScene().sceneRegistry().find<ParticleResources>())
            renderScene().sceneRegistry().emplace<ParticleResources>();

        if (auto* res = particleResources()) {
            res->setDeferredQueue(&ctx.deferredDestroyQueue());

            ParticleResources::InitInfo info{};
            info.device_ctx      = &ctx.deviceContext();
            info.max_particles   = 8192;
            info.descriptor_pool = ctx.resourceContext().descriptorPool();
            info.descriptor_layout = ctx.descriptorLayouts().getParticleSetLayout();
            (void)res->init(info);
        }
    }
}

void ParticleFeature::extractRenderObjects(
    const RenderObjectExtractContext& ctx,
    std::vector<DrawPacket>& out_packets)
{
    (void)ctx;
    (void)out_packets;
}

// ---------------------------------------------------------------------------
// Phase 4 — addPasses  (global: compute simulation only)
// ---------------------------------------------------------------------------
void ParticleFeature::addPasses(RGBuilder& builder)
{
    auto* particle_res = particleResources();
    if (!particle_res || !particle_res->initialized()) return;
    auto& rctx = renderContext();

    // Import the particle SSBO as an RG buffer resource so the graph can
    // insert the compute→vertex barrier automatically.
    RGBufferDescription buf_desc{};
    buf_desc.size          = static_cast<uint64_t>(particle_res->maxParticles()) * sizeof(ParticleGpu);
    buf_desc.stride        = sizeof(ParticleGpu);
    buf_desc.element_count = particle_res->maxParticles();

    RGImportedBufferInfo buf_import{};
    buf_import.buffer_getter = [this](VkBuffer* out_buffers, uint32_t capacity) -> uint32_t {
        if (out_buffers == nullptr || capacity == 0)
            return 0u;
        auto* pr = particleResources();
        if (!pr || !pr->initialized())
            return 0u;
        out_buffers[0] = pr->particleBuffer();
        return 1u;
    };

    particle_buf_rg_ = builder.importBuffer("ParticleSSBO", buf_desc, buf_import);

    // Pass 1: run the simulation compute shader over all particles.
    builder.addPass("ParticleUpdate", ERGPassType::COMPUTE)
        .setComputePipeline(compute_pipeline_handle_)
        .write(particle_buf_rg_, ERGBufferRole::STORAGE)
        .bindImmutableDS(0, particle_res->descriptorSet())
        .setKernelFn([this](const PassRecordContext& ctx) {
            auto* particle_res = particleResources();
            if (!particle_res || !particle_res->initialized()) return;
            if (ctx.pipeline_layout == VK_NULL_HANDLE) return;

            struct PC { float dt; float gravity; uint32_t max_particles; float _pad; };
            PC pc{ delta_time_, gravity_, particle_res->maxParticles(), 0.f };
            vkCmdPushConstants(ctx.cmd, ctx.pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT,
                               0, sizeof(PC), &pc);

            const uint32_t groups = (particle_res->maxParticles() + 63u) / 64u;
            vkCmdDispatch(ctx.cmd, groups, 1, 1);
        })
        .setKernel("ParticleUpdate");

    // Pass 2: draw billboard quads with additive blending on top of opaque geometry.
    builder.addPass("ParticleDraw", ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(cfg_.color_target),  lux::common::ETextureRole::COLOR_ATTACHMENT)
        .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
        .read(particle_buf_rg_, ERGBufferRole::STORAGE)
        .setPipeline(draw_pipeline_handle_)
        .bindSceneDS(0)
        .bindImmutableDS(1, renderScene().sceneRegistry().descriptorSetOf<InstanceResources>())
        .bindImmutableDS(2, rctx.globalRegistry().descriptorSetOf<TextureResources>())
        .bindResourceDS(3, renderScene().sceneRegistry().find<LightResources>(), &LightResources::resolveDS, EDSBindMode::PER_FIF,
                        builder.trackExternalBuffer("ext.LightResources"), ERGResourceType::BUFFER)
        .bindResourceDS(4, rctx.globalRegistry().find<MaterialResources>(), &MaterialResources::resolveDS, EDSBindMode::PER_FIF,
                        builder.trackExternalBuffer("ext.MaterialResources"), ERGResourceType::BUFFER)
        .bindImmutableDS(5, particle_res->descriptorSet())
        .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardTrans)))
        .setKernelFn([this](const PassRecordContext& ctx) {
            auto* particle_res = particleResources();
            if (!particle_res || !particle_res->initialized()) return;
            if (ctx.view == nullptr) return;

            const uint32_t particle_count = particle_res->maxParticles();
            if (particle_count == 0)
                return;

            vkCmdDraw(ctx.cmd, 6, particle_count, 0, 0);
        })
        .setKernel("ParticleDraw");
}



} // namespace lux::render
