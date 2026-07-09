// ============================================================================
//  Canvas2DOperationHandlers.cpp — Canvas2DFeature factory + the v2 GPU-driven
//  instance-command handlers, registered with DYNAMIC TypeIds via
//  register_ops_fn (the grid / meshstack pattern). The core RenderServer
//  dispatcher never names Canvas2D.
//
//  Every handler resolves the target scene's Canvas2DInstanceArena and applies
//  one command; a missing scene / feature / arena is inert (AddSprite2D reports
//  it as InvalidConfiguration through its reply — G-05; the fire-and-forget ops
//  simply no-op). Handle staleness is the ARENA's job (generational slots).
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>            // Dispatcher, Ctx, replyToCurrent
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>      // typed-op register/unregister
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>     // send / sendWithReply / sendBulk
#include <lux/engine/render/comm/RenderProtocol.hpp>                 // FeatureFactory
#include <lux/engine/render/core/FeatureDescriptor.hpp>              // FeatureDescriptor / featureId
#include <lux/engine/render/comm/client/RenderSession.hpp>           // Canvas2DProxy send path
#include <lux/engine/render/scene/RenderScene.hpp>                   // sceneRegistry().find

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeature.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DInstanceArena.hpp>

#include <cstring>
#include <span>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    // Exported by the server for feature operation handlers (grid convention).
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        [[nodiscard]] Canvas2DInstanceArena* resolveArena(Ctx& ctx, RenderSceneId scene)
        {
            auto* sc = lookupScene(ctx.user_state, scene);
            if (!sc) return nullptr;
            auto* arena = sc->sceneRegistry().find<Canvas2DInstanceArena>();
            return (arena && arena->initialized()) ? arena : nullptr;
        }

        void handleAddSprite2D(Ctx& ctx, const AddSprite2DPayload& p)
        {
            Sprite2DSlotReply r{};
            if (auto* arena = resolveArena(ctx, p.scene))
                r.status = arena->add(p.data, p.priority, p.visible != 0, r.handle);
            else
                r.status = ECanvas2DCreateStatus::InvalidConfiguration;
            replyToCurrent<AddSprite2DPayload>(ctx, r);
        }

        void handleRemoveSprite2D(Ctx& ctx, const RemoveSprite2DPayload& p)
        {
            if (auto* arena = resolveArena(ctx, p.scene))
                arena->remove(p.handle);
        }

        // Per-frame dirty deltas — each entry self-routes by its own scene (G-04).
        void handleSprite2DTransformBatch(Ctx& ctx, std::span<const Sprite2DTransformEntry> entries)
        {
            for (const auto& e : entries)
                if (auto* arena = resolveArena(ctx, e.scene))
                    arena->writeTransform(e.handle, e.m);
        }

        void handleUpdateSprite2DVisual(Ctx& ctx, const UpdateSprite2DVisualPayload& p)
        {
            if (auto* arena = resolveArena(ctx, p.scene))
                arena->writeVisual(p.handle, p.uv, p.tint, p.texture_bindless);
        }

        void handleUpdateSprite2DKey(Ctx& ctx, const UpdateSprite2DKeyPayload& p)
        {
            if (auto* arena = resolveArena(ctx, p.scene))
                arena->writeKey(p.handle, p.priority, p.visible != 0);
        }

        void handleSetCanvas2DEnabled(Ctx& ctx, const SetCanvas2DEnabledPayload& p)
        {
            if (auto* arena = resolveArena(ctx, p.scene))
                arena->setEnabled(p.enabled != 0);
        }

        // ── PixelField kind (F2-09) ──
        void handleAddPixelField2D(Ctx& ctx, const AddPixelField2DPayload& p)
        {
            PixelFieldSlotReply r{};
            if (auto* arena = resolveArena(ctx, p.scene))
                r.status = arena->addField(p.data, p.priority, p.visible != 0, r.handle);
            else
                r.status = ECanvas2DCreateStatus::InvalidConfiguration;
            replyToCurrent<AddPixelField2DPayload>(ctx, r);
        }

        void handleRemovePixelField2D(Ctx& ctx, const RemovePixelField2DPayload& p)
        {
            if (auto* arena = resolveArena(ctx, p.scene))
                arena->removeField(p.handle);
        }

        void handleUpdatePixelField2DTransform(Ctx& ctx, const UpdatePixelField2DTransformPayload& p)
        {
            if (auto* arena = resolveArena(ctx, p.scene))
                arena->writeFieldTransform(p.handle, p.m);
        }

        void handleUpdatePixelField2DKey(Ctx& ctx, const UpdatePixelField2DKeyPayload& p)
        {
            if (auto* arena = resolveArena(ctx, p.scene))
                arena->writeFieldKey(p.handle, p.priority, p.visible != 0);
        }
    } // namespace

    // ── Uniform factory interface ────────────────────────────────────────────
    static FeatureHandle canvas2dCreateFn(void* scene_ptr, const void* /*param*/, std::size_t /*sz*/)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        return sc->addFeature<Canvas2DFeature>(Canvas2DFeature::Config{});
    }

    // Typed-op register/unregister generated from the op list. The declared
    // order == Canvas2DOperationIds order on the client.
    using Canvas2DOps = FeatureOpRegistrar<
        ServerOp<AddSprite2DOp,                 &handleAddSprite2D>,
        ServerOp<RemoveSprite2DOp,              &handleRemoveSprite2D>,
        ServerOp<Sprite2DTransformBatchOp,      &handleSprite2DTransformBatch>,
        ServerOp<UpdateSprite2DVisualOp,        &handleUpdateSprite2DVisual>,
        ServerOp<UpdateSprite2DKeyOp,           &handleUpdateSprite2DKey>,
        ServerOp<SetCanvas2DEnabledOp,          &handleSetCanvas2DEnabled>,
        ServerOp<AddPixelField2DOp,             &handleAddPixelField2D>,
        ServerOp<RemovePixelField2DOp,          &handleRemovePixelField2D>,
        ServerOp<UpdatePixelField2DTransformOp, &handleUpdatePixelField2DTransform>,
        ServerOp<UpdatePixelField2DKeyOp,       &handleUpdatePixelField2DKey>>;

    // Stable type identity + descriptor. v2 bumps the stable id (the wire
    // protocol is incompatible with v1's submit model); the NAME stays
    // "Canvas2D" — clients resolve ops by name.
    static constexpr FeatureDescriptor kCanvas2DDescriptor{
        .type               = featureId("lux.render.canvas2d.v2"),
        .name               = "Canvas2D",
        .contributes_graph  = true,
        .creates_view_state = false,
        .multiplicity       = FeatureMultiplicity::SinglePerScene,
    };

    const FeatureFactory kCanvas2DFeatureFactory{
        &canvas2dCreateFn,
        &Canvas2DOps::registerAll,
        &Canvas2DOps::unregisterAll,
        "Canvas2D",
        -1,
        kCanvas2DDescriptor,
    };

    // ── Client-side proxy (typed-op send routing) ─────────────────────────────

    RenderRequest<Sprite2DSlotReply> 
    Canvas2DProxy::addSprite(RenderSceneId scene, const Sprite2DInstanceData& data, float priority, bool visible)
    {
        AddSprite2DPayload p{};
        p.scene    = scene;
        p.data     = data;
        p.priority = priority;
        p.visible  = visible ? 1u : 0u;
        return sendWithReply<AddSprite2DOp>(*session_, ops_, p);
    }

    void Canvas2DProxy::removeSprite(RenderSceneId scene, Sprite2DHandle handle)
    {
        if (!ops_.valid()) return;
        RemoveSprite2DPayload p{};
        p.scene  = scene;
        p.handle = handle;
        send<RemoveSprite2DOp>(*session_, ops_, p);
    }

    void Canvas2DProxy::updateTransforms(std::span<const Sprite2DTransformEntry> entries)
    {
        if (!ops_.valid() || entries.empty()) return;
        sendBulk<Sprite2DTransformBatchOp>(*session_, ops_, entries);
    }

    void Canvas2DProxy::updateTransforms(RenderSceneId scene, std::span<Sprite2DTransformEntry> entries)
    {
        if (!ops_.valid() || entries.empty()) return;
        for (auto& e : entries)
            e.scene = scene;
        sendBulk<Sprite2DTransformBatchOp>(*session_, ops_,
            std::span<const Sprite2DTransformEntry>{entries.data(), entries.size()});
    }

    void Canvas2DProxy::updateTransform(RenderSceneId scene, Sprite2DHandle handle, const float m[6])
    {
        if (!ops_.valid()) return;
        Sprite2DTransformEntry e{};
        e.scene  = scene;
        e.handle = handle;
        std::memcpy(e.m, m, sizeof(e.m));
        sendBulk<Sprite2DTransformBatchOp>(*session_, ops_,
            std::span<const Sprite2DTransformEntry>{&e, 1});
    }

    void Canvas2DProxy::updateVisual(RenderSceneId scene, Sprite2DHandle handle,
                                     const float uv[4], std::uint32_t tint,
                                     std::uint32_t texture_bindless)
    {
        if (!ops_.valid()) return;
        UpdateSprite2DVisualPayload p{};
        p.scene  = scene;
        p.handle = handle;
        std::memcpy(p.uv, uv, sizeof(p.uv));
        p.tint             = tint;
        p.texture_bindless = texture_bindless;
        send<UpdateSprite2DVisualOp>(*session_, ops_, p);
    }

    void Canvas2DProxy::updateKey(RenderSceneId scene, Sprite2DHandle handle,
                                  float priority, bool visible)
    {
        if (!ops_.valid()) return;
        UpdateSprite2DKeyPayload p{};
        p.scene    = scene;
        p.handle   = handle;
        p.priority = priority;
        p.visible  = visible ? 1u : 0u;
        send<UpdateSprite2DKeyOp>(*session_, ops_, p);
    }

    void Canvas2DProxy::setEnabled(RenderSceneId scene, bool enabled)
    {
        if (!ops_.valid()) return;
        SetCanvas2DEnabledPayload p{};
        p.scene   = scene;
        p.enabled = enabled ? 1u : 0u;
        send<SetCanvas2DEnabledOp>(*session_, ops_, p);
    }

    // ── PixelField kind (F2-09) ──

    RenderRequest<PixelFieldSlotReply> Canvas2DProxy::addPixelField(
        RenderSceneId scene, const PixelField2DInstanceData& data, float priority, bool visible)
    {
        AddPixelField2DPayload p{};
        p.scene    = scene;
        p.data     = data;
        p.priority = priority;
        p.visible  = visible ? 1u : 0u;
        return sendWithReply<AddPixelField2DOp>(*session_, ops_, p);
    }

    void Canvas2DProxy::removePixelField(RenderSceneId scene, PixelFieldInstanceHandle handle)
    {
        if (!ops_.valid()) return;
        RemovePixelField2DPayload p{};
        p.scene  = scene;
        p.handle = handle;
        send<RemovePixelField2DOp>(*session_, ops_, p);
    }

    void Canvas2DProxy::updatePixelFieldTransform(RenderSceneId scene,
                                                  PixelFieldInstanceHandle handle, const float m[6])
    {
        if (!ops_.valid()) return;
        UpdatePixelField2DTransformPayload p{};
        p.scene  = scene;
        p.handle = handle;
        std::memcpy(p.m, m, sizeof(p.m));
        send<UpdatePixelField2DTransformOp>(*session_, ops_, p);
    }

    void Canvas2DProxy::updatePixelFieldKey(RenderSceneId scene,
                                            PixelFieldInstanceHandle handle,
                                            float priority, bool visible)
    {
        if (!ops_.valid()) return;
        UpdatePixelField2DKeyPayload p{};
        p.scene    = scene;
        p.handle   = handle;
        p.priority = priority;
        p.visible  = visible ? 1u : 0u;
        send<UpdatePixelField2DKeyOp>(*session_, ops_, p);
    }

} // namespace lux::render
