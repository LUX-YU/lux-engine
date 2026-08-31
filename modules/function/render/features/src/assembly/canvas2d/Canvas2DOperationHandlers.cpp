// ============================================================================
//  Canvas2DOperationHandlers.cpp — Canvas2DFeature factory + the v2 GPU-driven
//  instance-command handlers, registered with DYNAMIC TypeIds via
//  register_ops_fn (the grid / meshstack pattern). The core RenderServer
//  dispatcher never names Canvas2D.
//
//  Every handler resolves the target scene's Canvas2DInstanceArena and applies
//  one command; a missing scene / feature / arena is inert (AddImage2D reports
//  it as InvalidConfiguration through its reply — G-05; the fire-and-forget ops
//  simply no-op). Handle staleness is the ARENA's job (generational slots).
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>                // Dispatcher, Ctx, replyToCurrent
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>          // typed-op register/unregister
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp> // FeatureFactory / GenericOkReply
#include <lux/engine/function/render/client/core/FeatureDescriptor.hpp>  // FeatureDescriptor / featureId
#include <lux/engine/render/scene/RenderScene.hpp>                       // resources().find

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeature.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DInstanceArena.hpp>

#include <cstring>
#include <span>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx = Dispatcher::Ctx;

    // Exported by the server for feature operation handlers (grid convention).
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        [[nodiscard]] Canvas2DInstanceArena* resolveArena(Ctx& ctx, RenderSceneId scene)
        {
            auto* sc = lookupScene(ctx.user_state, scene);
            if (!sc)
                return nullptr;
            auto* arena = sc->resources().find<Canvas2DInstanceArena>();
            return (arena && arena->initialized()) ? arena : nullptr;
        }
    } // namespace

    // handler 必须是**外部链接**:生成的 registrar 按 handle<OpName> 约定取地址
    // (缺了就是链接错)。与 Grid3D 等已迁特性同形。

    void handleAddImage2D(Ctx& ctx, const AddImage2DPayload& p)
    {
        Image2DSlotReply r{};
        if (auto* arena = resolveArena(ctx, p.scene))
            r.status = arena->add(p.data, p.priority, p.visible != 0, r.handle, p.group);
        else
            r.status = ECanvas2DCreateStatus::InvalidConfiguration;
        replyToCurrent<AddImage2DPayload>(ctx, r);
    }

    void handleRemoveImage2D(Ctx& ctx, const RemoveImage2DPayload& p)
    {
        if (auto* arena = resolveArena(ctx, p.scene))
            arena->remove(p.handle);
    }

    // Per-frame dirty deltas — each entry self-routes by its own scene (G-04).
    void handleImage2DTransformBatch(Ctx& ctx, std::span<const Image2DTransformEntry> entries)
    {
        for (const auto& e : entries)
            if (auto* arena = resolveArena(ctx, e.scene))
                arena->writeTransform(e.handle, e.m, e.page_delta);
    }

    void handleUpdateImage2DVisual(Ctx& ctx, const UpdateImage2DVisualPayload& p)
    {
        if (auto* arena = resolveArena(ctx, p.scene))
            arena->writeVisual(p.handle, p.uv, p.tint, p.texture_bindless);
    }

    void handleUpdateImage2DKey(Ctx& ctx, const UpdateImage2DKeyPayload& p)
    {
        if (auto* arena = resolveArena(ctx, p.scene))
            arena->writeKey(p.handle, p.priority, p.visible != 0, p.group);
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
            arena->writeFieldTransform(p.handle, p.m, p.page_delta);
    }

    void handleUpdatePixelField2DKey(Ctx& ctx, const UpdatePixelField2DKeyPayload& p)
    {
        if (auto* arena = resolveArena(ctx, p.scene))
            arena->writeFieldKey(p.handle, p.priority, p.visible != 0);
    }

    // ── Tile kind (A2-02) ──

    void handleAddTile2D(Ctx& ctx, const AddTile2DPayload& p)
    {
        Tile2DSlotReply r{};
        if (auto* arena = resolveArena(ctx, p.scene))
            r.status = arena->addTile(p.data, p.priority, p.visible != 0, r.handle);
        else
            r.status = ECanvas2DCreateStatus::InvalidConfiguration;
        replyToCurrent<AddTile2DPayload>(ctx, r);
    }

    void handleRemoveTile2D(Ctx& ctx, const RemoveTile2DPayload& p)
    {
        if (auto* arena = resolveArena(ctx, p.scene))
            arena->removeTile(p.handle);
    }

    void handleUpdateTile2DTransform(Ctx& ctx, const UpdateTile2DTransformPayload& p)
    {
        if (auto* arena = resolveArena(ctx, p.scene))
            arena->writeTileTransform(p.handle, p.m, p.page_delta);
    }

    void handleUpdateTile2DKey(Ctx& ctx, const UpdateTile2DKeyPayload& p)
    {
        if (auto* arena = resolveArena(ctx, p.scene))
            arena->writeTileKey(p.handle, p.priority, p.visible != 0);
    }

} // namespace lux::render
