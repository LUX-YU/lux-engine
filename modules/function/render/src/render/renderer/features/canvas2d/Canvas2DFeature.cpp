// ============================================================================
//  Canvas2DFeature.cpp — 2D draw-batch owner: sort, upload, draw sprites.
//
//  Owns Canvas2DResources via the scene registry (the feature-owned-resource pattern)
//  plus a host-mapped vertex ring with one slot per frame-in-flight. Each frame it
//  drains the submitted SpriteDraws, painter-order sorts them, CPU-expands them into the
//  slot for the current frame_index, and draws them color-only to SceneColor (no depth).
//  The 2D camera's ortho view/proj is read from the standard per-view scene descriptor
//  set (set 0, binding 1); textures from the global bindless set (set 2).
// ============================================================================

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeature.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DResources.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DSort.hpp>       // sortDraws

#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/resources/EBuiltinShader.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>   // bindless combined-sampler set (set 2)
#include <lux/engine/render/pipeline/GraphicsPipelineTemplate.hpp>

#include <vk_mem_alloc.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <system_error>
#include <utility>
#include <vector>

namespace lux::render
{
namespace
{
    /// Canvas2D sprite pipeline preset: CanvasVertex layout, PREMULTIPLIED-alpha blend
    /// (src=ONE, NOT SRC_ALPHA), NO depth (pure 2D painter order), no culling.
    GraphicsPipelineTemplate makeCanvas2DSpriteTemplate()
    {
        GraphicsPipelineTemplate tmpl{};

        VkVertexInputBindingDescription b{};
        b.binding   = 0;
        b.stride    = static_cast<uint32_t>(sizeof(CanvasVertex));
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        tmpl.vertex_bindings = { b };

        tmpl.vertex_attributes = {
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(CanvasVertex, x)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(CanvasVertex, u)},
            VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32_UINT,      offsetof(CanvasVertex, rgba)},
            VkVertexInputAttributeDescription{3, 0, VK_FORMAT_R32_UINT,      offsetof(CanvasVertex, texture_bindless)},
        };

        tmpl.topology            = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        tmpl.polygon_mode        = VK_POLYGON_MODE_FILL;
        tmpl.cull_mode           = VK_CULL_MODE_NONE;
        tmpl.front_face          = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        // Pure 2D painter order: draw-order IS the depth order → depth fully off.
        tmpl.depth_test_enable   = VK_FALSE;
        tmpl.depth_write_enable  = VK_FALSE;

        // Premultiplied alpha (design §3.3 MVP): dst = src.rgb + dst.rgb*(1-src.a).
        tmpl.blend_enable            = VK_TRUE;
        tmpl.src_color_blend_factor  = VK_BLEND_FACTOR_ONE;
        tmpl.dst_color_blend_factor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tmpl.color_blend_op          = VK_BLEND_OP_ADD;
        tmpl.src_alpha_blend_factor  = VK_BLEND_FACTOR_ONE;
        tmpl.dst_alpha_blend_factor  = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        tmpl.alpha_blend_op          = VK_BLEND_OP_ADD;
        tmpl.color_write_mask        = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        tmpl.use_dynamic_viewport = true;
        tmpl.use_dynamic_scissor  = true;
        return tmpl;
    }

    /// CPU-expand one SpriteDraw into 6 CanvasVertex (2 triangles), applying its
    /// column-major world 4x4 to a unit quad centred at the origin. z is 0 (the shader
    /// re-inserts it), so only the top-left 2x2 + translation columns matter.
    void expandSprite(const SpriteDraw& s, CanvasVertex* out)
    {
        const float* m = s.transform;   // column-major: element(row r, col c) = m[c*4+r]
        auto W = [&](float lx, float ly, CanvasVertex& v)
        {
            v.x = m[0] * lx + m[4] * ly + m[12];
            v.y = m[1] * lx + m[5] * ly + m[13];
        };
        const float ux0 = s.uv.x,          uy0 = s.uv.y;
        const float ux1 = s.uv.x + s.uv.w, uy1 = s.uv.y + s.uv.h;

        const std::uint32_t tex = s.texture_bindless;   // per-sprite bindless index (or kNoTexture)
        CanvasVertex bl, br, tr, tl;
        W(-0.5f, -0.5f, bl); bl.u = ux0; bl.v = uy1; bl.rgba = s.tint; bl.texture_bindless = tex;   // bottom-left
        W( 0.5f, -0.5f, br); br.u = ux1; br.v = uy1; br.rgba = s.tint; br.texture_bindless = tex;   // bottom-right
        W( 0.5f,  0.5f, tr); tr.u = ux1; tr.v = uy0; tr.rgba = s.tint; tr.texture_bindless = tex;   // top-right
        W(-0.5f,  0.5f, tl); tl.u = ux0; tl.v = uy0; tl.rgba = s.tint; tl.texture_bindless = tex;   // top-left

        out[0] = bl; out[1] = br; out[2] = tr;   // triangle 1
        out[3] = bl; out[4] = tr; out[5] = tl;   // triangle 2
    }
} // namespace

Canvas2DFeature::Canvas2DFeature(Config cfg)
    : RenderFeature(RenderFeature::Config{cfg.name})
    , cfg_(std::move(cfg))
{}

Canvas2DFeature::~Canvas2DFeature()
{
    destroySlotBuffers();
}

lux::render::Expected<void> Canvas2DFeature::initAndAttachTo(RenderScene& scene)
{
    using lux::cxx::unexpected;
    const auto fail = [](std::errc e) { return unexpected(std::make_error_code(e)); };

    // Own the scene's Canvas2DResources (per-frame draw snapshot ingest). Pure CPU state —
    // no 3D mesh arena is touched (a Canvas-only scene stays 3D-free).
    ingest_ = scene.sceneRegistry().ensure<Canvas2DResources>();

    // Shaders + pipeline (self-contained via the narrow RenderContextView SDK). Every
    // step is validated so a failed attach aborts + rolls back (initAndAttachTo is a
    // fallible Expected; the install path abandons the feature on unexpected).
    auto cv = contextView();
    cfg_.vertex_shader   = cv.loadBuiltinShader(EBuiltinShader::CANVAS2D_SPRITE_VERT, cfg_.vertex_shader);
    cfg_.fragment_shader = cv.loadBuiltinShader(EBuiltinShader::CANVAS2D_SPRITE_FRAG, cfg_.fragment_shader);

    auto tmpl = makeCanvas2DSpriteTemplate();
    // set 0 (scene camera) + set 2 (bindless textures). Set 1 (Instance) is in the
    // standard layout but UNUSED by the sprite shader → left unbound (a 2D-only scene has
    // no InstanceResources; the layout only needs the static set-1 SHAPE, not its data).
    tmpl.descriptor_set_count = 3;
    tmpl.vertex_shader   = cv.shaderModule(cfg_.vertex_shader);
    tmpl.fragment_shader = cv.shaderModule(cfg_.fragment_shader);
    if (tmpl.vertex_shader == VK_NULL_HANDLE || tmpl.fragment_shader == VK_NULL_HANDLE)
        return fail(std::errc::io_error);   // builtin sprite shader missing / failed to load

    const std::vector<const lux::rdesc::ShaderInfo*> infos = {
        cv.shaderInfo(cfg_.vertex_shader),
        cv.shaderInfo(cfg_.fragment_shader),
    };
    tmpl.pipeline_layout = cv.buildStandardGraphicsLayout(
        tmpl.descriptor_set_count, infos, "Canvas2DSpriteLayout");
    if (tmpl.pipeline_layout == VK_NULL_HANDLE)
        return fail(std::errc::io_error);   // layout build failed

    pipeline_handle_ = cv.registerGraphics(tmpl, infos);
    if (pipeline_handle_ == kInvalidPipelineHandle)
        return fail(std::errc::io_error);   // pipeline registration failed

    // One host-mapped ring slot per frame-in-flight (rolls back partial allocations).
    allocator_ = cv.vmaAllocator();
    if (auto r = createSlotBuffers(cv.framesInFlight()); !r)
        return r;

    return {};
}

void Canvas2DFeature::onFrameBegin(const FeatureFrameContext& ctx)
{
    draw_count_ = 0;
    stats_ = {};
    if (slots_.empty() || !ingest_) return;

    // Pick the ring slot for the engine's REAL frame-in-flight — never a private counter,
    // which could overwrite a slot the GPU is still reading.
    active_slot_ = ctx.frame_index % static_cast<std::uint32_t>(slots_.size());

    ingest_->drainInto(sprite_snapshot_);   // swap-in (keeps capacity on both buffers)
    if (sprite_snapshot_.empty()) return;

    // Painter-order sort the whole list so the expand+upload below emit vertices in draw
    // order (bindless per-sprite texture selection needs no per-texture batching).
    sortDraws(sprite_snapshot_);

    const std::uint32_t total = static_cast<std::uint32_t>(sprite_snapshot_.size());
    const std::uint32_t n     = std::min(total, cfg_.max_sprites_per_frame);

    auto& slot = slots_[active_slot_];
    auto* out  = static_cast<CanvasVertex*>(slot.mapped);
    for (std::uint32_t i = 0; i < n; ++i)
        expandSprite(sprite_snapshot_[i], out + i * kVertsPerSprite);

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(n) * kVertsPerSprite * sizeof(CanvasVertex);
    vmaFlushAllocation(allocator_, slot.alloc, 0, bytes);
    draw_count_ = n * kVertsPerSprite;

    // Non-file-IO diagnostic of the over-budget drop (stats, not a render-thread log).
    stats_.sprites = n;
    stats_.dropped = total - n;
}

void Canvas2DFeature::addPasses(RGBuilder& builder)
{
    // ONE stable pass — topology fixed at build time; only draw_count_ (content) varies
    // per frame, so sprite updates never invalidate the graph (R2-02 acceptance).
    // Color-only (no depth attachment): 2D painter order comes from CPU draw order.
    builder.addPass("Canvas2D", ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
        .setPipeline(pipeline_handle_)
        .bindSceneDS(0)
        // set 2 = the global bindless combined-sampler atlas (domain-neutral TextureResources,
        // the SAME set the 3D material path samples). Set 1 (Instance) is intentionally NOT
        // bound — the sprite shader never uses it, so a 2D-only scene stays InstanceResources-free.
        .bindImmutableDS(2, contextView().globalRegistry().descriptorSetOf<TextureResources>())
        .setPhaseMask(phaseBit(static_cast<render_phase_id>(ECoreRenderPhase::ForwardTrans)))
        .setKernelFn([this](const PassRecordContext& ctx)
        {
            if (draw_count_ == 0) return;      // empty draw list → safe no-op
            if (ctx.view == nullptr) return;

            auto& slot = slots_[active_slot_];
            VkDeviceSize zero_offset = 0;
            vkCmdBindVertexBuffers(ctx.cmd, 0, 1, &slot.buffer, &zero_offset);
            vkCmdDraw(ctx.cmd, draw_count_, 1, 0, 0);
        })
        .setKernel("Canvas2DDraw")
        .stage(ERenderStage::Transparent);   // coarse "World2D" stage (design §3.3): blended
                                             // scene content, before PostProcess
}

void Canvas2DFeature::onDetachFromScene(RenderScene& /*scene*/)
{
    // Canvas2DResources is registry-owned (freed at scene teardown). The ring buffers
    // are feature-owned: runtime removeFeature has no GPU idle wait, so retire them
    // through the FIF deferred-destroy queue (frames N-1/N-2 may still bind them).
    // Nulling the handles makes destroySlotBuffers() a no-op on this path.
    auto cv = contextView();
    for (auto& slot : slots_)
    {
        if (slot.buffer != VK_NULL_HANDLE)
            cv.retireBuffer(slot.buffer, slot.alloc);
        slot = {};
    }
}

lux::render::Expected<void> Canvas2DFeature::createSlotBuffers(std::uint32_t frames_in_flight)
{
    const std::uint32_t count = std::max<std::uint32_t>(1u, frames_in_flight);
    const VkDeviceSize byte_size =
        static_cast<VkDeviceSize>(cfg_.max_sprites_per_frame) * kVertsPerSprite * sizeof(CanvasVertex);

    slots_.assign(count, FrameSlot{});
    for (std::uint32_t i = 0; i < count; ++i)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size  = byte_size;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        const VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &slots_[i].buffer, &slots_[i].alloc, &info);
        if (r != VK_SUCCESS || info.pMappedData == nullptr)
        {
            // Roll back the buffers created so far (0..i-1); slot i is null on failure.
            destroySlotBuffers();
            slots_.clear();
            return lux::cxx::unexpected(std::make_error_code(std::errc::not_enough_memory));
        }
        slots_[i].mapped = info.pMappedData;
    }
    return {};
}

void Canvas2DFeature::destroySlotBuffers()
{
    // Fallback for the no-detach path (device idle at shutdown); a no-op after
    // onDetachFromScene() has retired + nulled the handles.
    if (!allocator_) return;
    for (auto& slot : slots_)
    {
        if (slot.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(allocator_, slot.buffer, slot.alloc);
        slot = {};
    }
}

} // namespace lux::render
