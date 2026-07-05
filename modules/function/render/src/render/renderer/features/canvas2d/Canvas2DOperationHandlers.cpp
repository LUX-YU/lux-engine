// ============================================================================
//  Canvas2DOperationHandlers.cpp — Canvas2DFeature factory + descriptor + the
//  feature-scoped submit-sprites handler, registered with a DYNAMIC TypeId via
//  register_ops_fn (the grid pattern). The core RenderServer dispatcher never
//  names Canvas2D. Mirrors LightOperationHandlers.cpp / FeatureParamsHandlers.cpp.
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>            // Dispatcher, Ctx, resolveBlob
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>      // typed-op register/unregister
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>     // send<Op>
#include <lux/engine/render/comm/RenderProtocol.hpp>                 // FeatureFactory, opcodes
#include <lux/engine/render/core/FeatureDescriptor.hpp>              // FeatureDescriptor / featureId / FeatureMultiplicity
#include <lux/engine/render/comm/client/RenderSession.hpp>           // Canvas2DProxy: builder / pushBlob
#include <lux/engine/render/scene/RenderScene.hpp>                   // sceneRegistry().find

#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeature.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DResources.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    // Exported by the server for feature operation handlers (forward-declared — the
    // grid-handler convention — to avoid pulling RenderServerImpl.hpp into a feature).
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        // Append a submitted sprite batch to the scene's Canvas2DResources. Routed by
        // scene_id (SinglePerScene → one Canvas per scene); no-op if the scene has no
        // Canvas2DFeature. The Blob is resolved to bytes and reinterpreted as SpriteDraw[]
        // (a trivially-copyable POD, R2-00), then COPIED in — no borrowed pointer survives.
        void handleCanvas2DSubmit(Ctx& ctx, const Canvas2DSubmitPayload& p)
        {
            auto* sc = lookupScene(ctx.user_state, p.scene);
            if (!sc) return;
            auto* res = sc->sceneRegistry().find<Canvas2DResources>();
            if (!res) return;

            const auto blob = resolveBlob(ctx.program, p.sprites);
            const std::size_t count = blob.size() / sizeof(SpriteDraw);
            if (count == 0) return;
            res->ingestSprites(std::span<const SpriteDraw>{
                reinterpret_cast<const SpriteDraw*>(blob.data()), count});
        }
    } // namespace

    // ── Uniform factory interface ────────────────────────────────────────────
    static FeatureHandle canvas2dCreateFn(void* scene_ptr, const void* /*param*/, std::size_t /*sz*/)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        return sc->addFeature<Canvas2DFeature>(Canvas2DFeature::Config{});
    }

    // Typed-op register/unregister generated from the op list. Blob → allocateAndRegisterUnary
    // on CommandOp; the declared order == Canvas2DOperationIds order on the client.
    using Canvas2DOps = FeatureOpRegistrar<
        ServerOp<Canvas2DSubmitOp, &handleCanvas2DSubmit>>;

    // Stable type identity + descriptor. R2-02: addPasses declares the Canvas SceneColor
    // pass, so contributes_graph=true (a feature-set change invalidates the graph — but
    // per-frame sprite CONTENT does not, since it flows through onFrameBegin, not addPasses).
    // SinglePerScene so a second Canvas per scene is rejected (design §0R V5).
    static constexpr FeatureDescriptor kCanvas2DDescriptor{
        .type               = featureId("lux.render.canvas2d.v1"),
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

    // ── Client-side proxy ─────────────────────────────────────────────────────
    void Canvas2DProxy::submitSprites(RenderSceneId scene, std::span<const SpriteDraw> sprites)
    {
        if (!ops_.valid() || sprites.empty()) return;
        auto& b = session_->builder();
        Canvas2DSubmitPayload p{};
        p.scene   = scene;
        p.sprites = b.pushBlob(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(sprites.data()), sprites.size_bytes()},
            alignof(SpriteDraw));
        send<Canvas2DSubmitOp>(*session_, ops_, p);
    }

} // namespace lux::render
