/**
 * @file TriOverlayOperationHandlers.cpp
 * @brief FeatureFactory and server-side operation handler for TriOverlayTransientFeature,
 *        plus client-side TriOverlayProxy implementation.
 */

#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/renderer/features/gizmo/TriOverlayOperation.hpp>
#include <lux/engine/render/renderer/features/gizmo/TriOverlayTransientFeature.hpp>
#include <lux/engine/render/renderer/features/gizmo/GizmoVertex.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <cassert>

namespace lux::render
{

// Defined in RenderServer.cpp
RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

namespace
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    // ── Transient handler: replace all triangles for this frame ──────────

    void handleReplaceTransientTriangles(Ctx& ctx, const UploadTriOverlayPayload& p)
    {
        auto* sc  = lookupScene(ctx.user_state, p.scene_id);
        auto* buf = sc ? sc->sceneRegistry().find<TransientTriOverlayBuffer>() : nullptr;
        if (!buf) {
            replyToCurrent<UploadTriOverlayPayload>(ctx,
                TriOverlayUploadedReply{p.chunk_id, 1u});
            return;
        }

        auto blob = resolveBlob(ctx.program, p.vertex_data);
        assert(blob.size() == p.vertex_count * sizeof(GizmoVertex));

        buf->replace(
            reinterpret_cast<const GizmoVertex*>(blob.data()),
            p.vertex_count);

        replyToCurrent<UploadTriOverlayPayload>(ctx,
            TriOverlayUploadedReply{p.chunk_id, 0u});
    }

} // anonymous namespace

    // Typed-op: register/unregister generated from the op list. The Blob handler
    // above (handleReplaceTransientTriangles) is the only hand-written piece.
    using TriOverlayOps = FeatureOpRegistrar<ServerOp<UploadTriOverlayOp, &handleReplaceTransientTriangles>>;

    // =====================================================================
    //  TriOverlayTransient factory
    // =====================================================================

    static uint32_t triOverlayTransientCreateFn(void* scene_ptr,
                                                const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        TriOverlayTransientCommConfig cc{};
        if (param && param_size >= sizeof(TriOverlayTransientCommConfig))
            cc = *static_cast<const TriOverlayTransientCommConfig*>(param);

        TriOverlayTransientFeature::Config cfg{};
        cfg.vertex_shader   = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.max_vertices    = cc.max_vertices;
        return sc->addFeature<TriOverlayTransientFeature>(cfg);
    }

    const FeatureFactory kTriOverlayTransientFactory{
        &triOverlayTransientCreateFn,
        &TriOverlayOps::registerAll,
        &TriOverlayOps::unregisterAll,
        "TriOverlayTransient",
    };

// =====================================================================
//  TriOverlayProxy — client-side proxy
// =====================================================================

RenderRequest<TriOverlayUploadedReply> TriOverlayProxy::uploadTriangles(
    RenderSceneId scene_id, uint32_t chunk_id,
    std::span<const GizmoVertex> vertices)
{
    UploadTriOverlayPayload payload{};
    payload.scene_id     = scene_id;
    payload.chunk_id     = chunk_id;
    payload.vertex_count = static_cast<uint32_t>(vertices.size());
    return sendBlob<UploadTriOverlayOp>(
        *session_, ops_, payload, std::as_bytes(vertices), alignof(GizmoVertex));
}

} // namespace lux::render
