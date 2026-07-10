// ============================================================================
//  Canvas2DFeature.cpp — GPU-driven 2D canvas (v2): arena wiring, the
//  vertex-pulling sprite pipeline, and the single instanced draw.
//
//  Per-frame there is NO feature-side work at all: the arena updates through
//  the scene's transfer phase (registered contributor), the pass kernel reads
//  drawCount() at record time. Content never touches the render graph.
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeature.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DInstanceArena.hpp>

#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/core/Errors.hpp>                    // ERenderError / make_error_code
#include <lux/engine/render/resources/EBuiltinShader.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>     // bindless combined-sampler set (set 2)
#include <lux/engine/render/resources/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/transfer/TransferContributor.hpp>   // makeTransferContributor

#include <cstdio>
#include <utility>

namespace lux::render
{
namespace
{
    /// v2 sprite pipeline preset: NO vertex input state (pure vertex pulling),
    /// PREMULTIPLIED-alpha blend (src=ONE), NO depth (painter order comes from
    /// the arena's order buffer), no culling.
    GraphicsPipelineTemplate makeCanvas2DSpriteTemplate()
    {
        GraphicsPipelineTemplate tmpl{};
        // vertex_bindings / vertex_attributes stay EMPTY on purpose — the VS
        // pulls instance data from set 1 and synthesizes the quad corners.

        tmpl.topology     = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        tmpl.polygon_mode = VK_POLYGON_MODE_FILL;
        tmpl.cull_mode    = VK_CULL_MODE_NONE;
        tmpl.front_face   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        // Draw-order IS the depth order → depth fully off.
        tmpl.depth_test_enable  = VK_FALSE;
        tmpl.depth_write_enable = VK_FALSE;

        // Premultiplied alpha: dst = src.rgb + dst.rgb*(1-src.a).
        tmpl.blend_enable           = VK_TRUE;
        tmpl.src_color_blend_factor = VK_BLEND_FACTOR_ONE;
        tmpl.dst_color_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tmpl.color_blend_op         = VK_BLEND_OP_ADD;
        tmpl.src_alpha_blend_factor = VK_BLEND_FACTOR_ONE;
        tmpl.dst_alpha_blend_factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tmpl.alpha_blend_op         = VK_BLEND_OP_ADD;
        tmpl.color_write_mask       = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        tmpl.use_dynamic_viewport = true;
        tmpl.use_dynamic_scissor  = true;
        return tmpl;
    }
} // namespace

// Out-of-line: needs the complete SceneDescriptorArena type (kept out of the
// arena header so it stays a plain resource header).
VkDescriptorSet Canvas2DInstanceArena::allocateSet(SceneDescriptorArena* arena,
                                                   VkDescriptorSetLayout layout)
{
    return arena ? arena->allocate(layout) : VK_NULL_HANDLE;
}

Canvas2DFeature::Canvas2DFeature(Config cfg)
    : RenderFeature(RenderFeature::Config{cfg.name})
    , cfg_(std::move(cfg))
{}

Canvas2DFeature::~Canvas2DFeature() = default;

lux::render::Expected<void> Canvas2DFeature::initAndAttachTo(RenderScene& scene)
{
    using lux::cxx::unexpected;
    const auto fail = [](ERenderError e) { return unexpected(make_error_code(e)); };

    auto& ctx = renderContext();

    // Own the scene's instance arena (registry lifetime = the scene; survives a
    // feature detach so a late command always lands somewhere stable). First
    // attach wires the deferred-destroy queue + the transfer contributor.
    {
        const bool fresh = (scene.sceneRegistry().find<Canvas2DInstanceArena>() == nullptr);
        arena_ = scene.sceneRegistry().ensure<Canvas2DInstanceArena>();
        if (fresh)
        {
            arena_->setDeferredQueue(&ctx.deferredDestroyQueue());
            scene.transferScheduler().contributors().add(
                makeTransferContributor(arena_, /*priority=*/1));
        }
        Canvas2DInstanceArena::InitInfo ii{};
        ii.device_context   = &ctx.deviceContext();
        ii.descriptor_svc   = &ctx.descriptorService();
        ii.arena            = &scene.descriptorArena();
        ii.initial_capacity = cfg_.initial_capacity;
        ii.max_capacity     = cfg_.max_capacity;
        ii.offscreen_groups = cfg_.offscreen_groups;
        arena_->init(ii);   // idempotent
        if (!arena_->initialized() ||
            arena_->descriptorSet(ECanvas2DKind::Sprite) == VK_NULL_HANDLE)
            return fail(ERenderError::VulkanObjectCreationFailed);
    }

    // Shaders + vertex-pulling pipelines — one per KIND, all sharing ONE pipeline
    // layout (set 0 scene/view, set 1 arena kind-store, set 2 bindless textures),
    // so the kernel's per-run switches rebind only pipeline + set 1. Every step
    // validated so a failed attach aborts.
    auto cv = contextView();
    cfg_.vertex_shader   = cv.loadBuiltinShader(EBuiltinShader::CANVAS2D_SPRITE_VERT, cfg_.vertex_shader);
    cfg_.fragment_shader = cv.loadBuiltinShader(EBuiltinShader::CANVAS2D_SPRITE_FRAG, cfg_.fragment_shader);
    const auto field_vert = cv.loadBuiltinShader(EBuiltinShader::CANVAS2D_FIELD_VERT, {});
    const auto field_frag = cv.loadBuiltinShader(EBuiltinShader::CANVAS2D_FIELD_FRAG, {});

    auto tmpl = makeCanvas2DSpriteTemplate();
    tmpl.descriptor_set_count = 3;
    tmpl.vertex_shader   = cv.shaderModule(cfg_.vertex_shader);
    tmpl.fragment_shader = cv.shaderModule(cfg_.fragment_shader);
    if (tmpl.vertex_shader == VK_NULL_HANDLE || tmpl.fragment_shader == VK_NULL_HANDLE)
        return fail(ERenderError::IOError);

    const std::vector<const lux::rdesc::ShaderInfo*> infos = {
        cv.shaderInfo(cfg_.vertex_shader),
        cv.shaderInfo(cfg_.fragment_shader),
    };
    tmpl.pipeline_layout = cv.buildStandardGraphicsLayout(
        tmpl.descriptor_set_count, infos, "Canvas2DSpriteLayout");
    if (tmpl.pipeline_layout == VK_NULL_HANDLE)
        return fail(ERenderError::VulkanObjectCreationFailed);

    pipeline_handle_ = cv.registerGraphics(tmpl, infos);
    if (pipeline_handle_ == kInvalidPipelineHandle)
        return fail(ERenderError::VulkanObjectCreationFailed);

    // PixelField kind pipeline: identical fixed state + layout, its own shaders.
    auto field_tmpl = makeCanvas2DSpriteTemplate();
    field_tmpl.descriptor_set_count = 3;
    field_tmpl.vertex_shader   = cv.shaderModule(field_vert);
    field_tmpl.fragment_shader = cv.shaderModule(field_frag);
    if (field_tmpl.vertex_shader == VK_NULL_HANDLE || field_tmpl.fragment_shader == VK_NULL_HANDLE)
        return fail(ERenderError::IOError);
    const std::vector<const lux::rdesc::ShaderInfo*> field_infos = {
        cv.shaderInfo(field_vert),
        cv.shaderInfo(field_frag),
    };
    field_tmpl.pipeline_layout = tmpl.pipeline_layout;   // SHARED layout (see above)
    field_pipeline_handle_ = cv.registerGraphics(field_tmpl, field_infos);
    if (field_pipeline_handle_ == kInvalidPipelineHandle)
        return fail(ERenderError::VulkanObjectCreationFailed);

    // Tile kind pipeline (A2-02): same recipe again.
    const auto tile_vert = cv.loadBuiltinShader(EBuiltinShader::CANVAS2D_TILE_VERT, {});
    const auto tile_frag = cv.loadBuiltinShader(EBuiltinShader::CANVAS2D_TILE_FRAG, {});
    auto tile_tmpl = makeCanvas2DSpriteTemplate();
    tile_tmpl.descriptor_set_count = 3;
    tile_tmpl.vertex_shader   = cv.shaderModule(tile_vert);
    tile_tmpl.fragment_shader = cv.shaderModule(tile_frag);
    if (tile_tmpl.vertex_shader == VK_NULL_HANDLE || tile_tmpl.fragment_shader == VK_NULL_HANDLE)
        return fail(ERenderError::IOError);
    const std::vector<const lux::rdesc::ShaderInfo*> tile_infos = {
        cv.shaderInfo(tile_vert),
        cv.shaderInfo(tile_frag),
    };
    tile_tmpl.pipeline_layout = tmpl.pipeline_layout;    // SHARED layout (see above)
    tile_pipeline_handle_ = cv.registerGraphics(tile_tmpl, tile_infos);
    if (tile_pipeline_handle_ == kInvalidPipelineHandle)
        return fail(ERenderError::VulkanObjectCreationFailed);

    // ── A2-04 composite pipeline (ONLY when groups are declared — a group-less
    // canvas pays nothing here). Fullscreen triangle (tonemap.vert) + a one-
    // sampler set 1; the canvas premultiplied blend composites the group RT
    // over color_target.
    if (cfg_.offscreen_groups > 0)
    {
        if (composite_ds_layout_ == VK_NULL_HANDLE)
        {
            const VkDescriptorSetLayoutBinding b{
                0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
            auto id = ctx.descriptorService().registerLayout(
                {.bindings = {&b, 1}, .debug_name = "Canvas2DGroupCompositeDS"});
            composite_ds_layout_ = ctx.descriptorService().layout(id);
        }
        if (group_sampler_.get() == VK_NULL_HANDLE)
        {
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = VK_FILTER_LINEAR;  si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            VkSampler s = VK_NULL_HANDLE;
            if (vkCreateSampler(ctx.deviceContext().logicalDevice(), &si, nullptr, &s) != VK_SUCCESS)
                return fail(ERenderError::VulkanObjectCreationFailed);
            group_sampler_ = FifOwned<VkSampler>{&ctx.deferredDestroyQueue(), s};
        }

        const auto fs_vert = cv.loadBuiltinShader(EBuiltinShader::TONEMAP_VERT, {});
        const auto co_frag = cv.loadBuiltinShader(EBuiltinShader::CANVAS2D_GROUP_COMPOSITE_FRAG, {});
        auto co_tmpl = makeCanvas2DSpriteTemplate();   // premultiplied blend, no depth
        co_tmpl.descriptor_set_count = 2;
        co_tmpl.vertex_shader   = cv.shaderModule(fs_vert);
        co_tmpl.fragment_shader = cv.shaderModule(co_frag);
        if (co_tmpl.vertex_shader == VK_NULL_HANDLE || co_tmpl.fragment_shader == VK_NULL_HANDLE)
            return fail(ERenderError::IOError);
        {
            const VkPushConstantRange pc{
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
            const std::array set_layouts{
                ctx.descriptorLayouts().getLayout(EDescriptorSetSlot::Scene),   // set 0
                composite_ds_layout_,                                           // set 1
            };
            const std::array pcs{pc};
            co_tmpl.pipeline_layout = ctx.pipelineLayoutService().getOrCreate(
                {.set_layouts = set_layouts, .push_constants = pcs,
                 .debug_name = "Canvas2DGroupCompositeLayout"}).value();
            co_tmpl.push_constant_ranges.assign(pcs.begin(), pcs.end());
            co_tmpl.resource_slot_map.push_back({EDescriptorSetSlot::Scene, 0});
        }
        const std::vector<const lux::rdesc::ShaderInfo*> co_infos = {
            cv.shaderInfo(fs_vert),
            cv.shaderInfo(co_frag),
        };
        composite_pipeline_ = cv.registerGraphics(co_tmpl, co_infos);
        if (composite_pipeline_ == kInvalidPipelineHandle)
            return fail(ERenderError::VulkanObjectCreationFailed);
    }

    return {};
}

void Canvas2DFeature::addPasses(RGBuilder& builder)
{
    // Stable topology fixed at build time; only the arena's CONTENT varies (via
    // the transfer phase), so sprite updates never touch the graph. With A2-04
    // groups declared, each group g gets its own full-view RT + draw pass +
    // premultiplied composite onto color_target — still all fixed per attach.

    // The shared run-walking kernel, filtered to ONE group: pipeline + set 1
    // switch only on kind changes; firstInstance addresses the kind's order
    // subsequence, so nothing else changes per group.
    const auto makeDrawKernel = [this](std::uint8_t group)
    {
        return [this, group](const PassRecordContext& ctx)
        {
            if (!arena_ || ctx.view == nullptr) return;
            if (group != 0)
            {
                // The graph clears color attachments to OPAQUE black; a group
                // RT must start TRANSPARENT (its composite is a premultiplied
                // over). Clear it here — local, no graph-core change.
                VkClearAttachment ca{};
                ca.aspectMask       = VK_IMAGE_ASPECT_COLOR_BIT;
                ca.colorAttachment  = 0;
                ca.clearValue.color = {{0.f, 0.f, 0.f, 0.f}};
                VkClearRect rect{};
                rect.rect       = {{0, 0}, ctx.extent};
                rect.layerCount = 1;
                vkCmdClearAttachments(ctx.cmd, 1, &ca, 1, &rect);
            }
            int bound_kind = static_cast<int>(ECanvas2DKind::Sprite);   // pass-level state
            for (const Canvas2DRun& run : arena_->runs())
            {
                if (run.group != group) continue;
                if (run.kind != bound_kind)
                {
                    // kind ids map 1:1 onto pipeline variants (Sprite=0,
                    // PixelField=1, Tile=2 — the setPipeline/addPipeline order).
                    ctx.bindPipelineVariant(run.kind);
                    const VkDescriptorSet ds =
                        arena_->descriptorSet(static_cast<ECanvas2DKind>(run.kind));
                    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            ctx.pipeline_layout, 1, 1, &ds, 0, nullptr);
                    bound_kind = run.kind;
                }
                vkCmdDraw(ctx.cmd, 6, run.count, 0, run.first);
            }
        };
    };

    // Group 0 — the direct pass (EXACTLY the pre-A2-04 pass when no groups).
    builder.addPass("Canvas2D", ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
        .setPipeline(pipeline_handle_)              // variant 0 = sprite kind
        .addPipeline(field_pipeline_handle_)        // variant 1 = pixel-field kind
        .addPipeline(tile_pipeline_handle_)         // variant 2 = tile kind
        .bindSceneDS(0)
        // set 1 pass-level bind = the sprite kind's set (the framework binds it
        // with variant 0 before the kernel); the kernel rebinds pipeline + set 1
        // PER RUN on kind changes (R6b) — every kind shares ONE pipeline layout,
        // so set 0 / set 2 stay bound across switches.
        .bindImmutableDS(1, arena_->descriptorSet(ECanvas2DKind::Sprite))
        // set 2 = the global bindless combined-sampler atlas (same set the 3D
        // material path samples).
        .bindImmutableDS(2, contextView().globalRegistry().descriptorSetOf<TextureResources>())
        .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardTrans)))
        .setKernelFn(makeDrawKernel(0))
        .setKernel("Canvas2DDraw")
        .stage(ERenderStage::Transparent);

    // Offscreen groups (A2-04): draw into a transient RGBA8 RT, then composite.
    const std::uint32_t groups = std::min(cfg_.offscreen_groups, kMaxCanvas2DGroups);
    for (std::uint32_t g = 1; g <= groups; ++g)
    {
        char rt_name[32];
        std::snprintf(rt_name, sizeof(rt_name), "Canvas2DGroup%u", g);
        char draw_name[40];
        std::snprintf(draw_name, sizeof(draw_name), "Canvas2DGroup%uDraw", g);
        char comp_name[40];
        std::snprintf(comp_name, sizeof(comp_name), "Canvas2DGroup%uComposite", g);
        char ds_name[48];
        std::snprintf(ds_name, sizeof(ds_name), "Canvas2DGroup%uCompositeDS", g);

        RGTextureDescription rt_desc = RGTextureDescription::Relative(
            1.0f, 1.0f, lux::common::ETextureFormat::RGBA8_UNORM);
        rt_desc.usage = static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT)
                      | static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED);
        auto rt_rg = builder.createTexture(rt_name, rt_desc);

        builder.addPass(draw_name, ERGPassType::GRAPHICS)
            .write(rt_rg, lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(pipeline_handle_)
            .addPipeline(field_pipeline_handle_)
            .addPipeline(tile_pipeline_handle_)
            .bindSceneDS(0)
            .bindImmutableDS(1, arena_->descriptorSet(ECanvas2DKind::Sprite))
            .bindImmutableDS(2, contextView().globalRegistry().descriptorSetOf<TextureResources>())
            .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardTrans)))
            .setKernelFn(makeDrawKernel(static_cast<std::uint8_t>(g)))
            .setKernel("Canvas2DDraw")
            .stage(ERenderStage::Transparent);

        auto comp_tds = builder.createTransientDS(ds_name, composite_ds_layout_, {
            {0, EDescriptorType::COMBINED_IMAGE_SAMPLER, rt_rg, group_sampler_.get(),
             EImageLayout::SHADER_READ_ONLY_OPTIMAL},
        });
        builder.addPass(comp_name, ERGPassType::GRAPHICS)
            .read(rt_rg, lux::common::ETextureRole::SAMPLED)
            .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(composite_pipeline_)
            .bindSceneDS(0)
            .bindTransientDS(1, comp_tds)
            .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardTrans)))
            .setKernelFn([](const PassRecordContext& rec)
            {
                vkCmdDraw(rec.cmd, 3, 1, 0, 0);   // fullscreen triangle
            })
            .setKernel("FullscreenQuad")
            .after("Canvas2D")                    // composite over the direct 2D content
            .stage(ERenderStage::Transparent);
    }
}

void Canvas2DFeature::onDetachFromScene(RenderScene& /*scene*/)
{
    // The arena is registry-owned: it survives a runtime detach (retained GPU
    // state, exactly like the 3D InstanceResources) and is torn down with the
    // scene — its streams route through the deferred-destroy queue. Nothing to
    // do here; the pipeline handle is registry-owned like every feature's.
    arena_ = nullptr;
}

} // namespace lux::render
