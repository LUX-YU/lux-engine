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
#include <lux/engine/render/transfer/TransferContributor.hpp>   // makeTransferContributor

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

    return {};
}

void Canvas2DFeature::addPasses(RGBuilder& builder)
{
    // ONE stable pass — topology fixed at build time; only the arena's CONTENT
    // varies (via the transfer phase), so sprite updates never touch the graph.
    builder.addPass("Canvas2D", ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
        .setPipeline(pipeline_handle_)              // variant 0 = sprite kind
        .addPipeline(field_pipeline_handle_)        // variant 1 = pixel-field kind
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
        .setKernelFn([this](const PassRecordContext& ctx)
        {
            if (!arena_ || ctx.view == nullptr) return;
            // Walk the run table: each run is one instanced draw over its kind's
            // order subsequence (firstInstance addresses it — the VS reads
            // order[gl_InstanceIndex]). Pipeline + set 1 switch ONLY on kind
            // changes; a single-kind frame is exactly one draw.
            int bound_kind = static_cast<int>(ECanvas2DKind::Sprite);   // pass-level state
            for (const Canvas2DRun& run : arena_->runs())
            {
                if (run.kind != bound_kind)
                {
                    ctx.bindPipelineVariant(run.kind == static_cast<std::uint8_t>(ECanvas2DKind::Sprite) ? 0u : 1u);
                    const VkDescriptorSet ds =
                        arena_->descriptorSet(static_cast<ECanvas2DKind>(run.kind));
                    vkCmdBindDescriptorSets(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            ctx.pipeline_layout, 1, 1, &ds, 0, nullptr);
                    bound_kind = run.kind;
                }
                vkCmdDraw(ctx.cmd, 6, run.count, 0, run.first);
            }
        })
        .setKernel("Canvas2DDraw")
        .stage(ERenderStage::Transparent);
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
