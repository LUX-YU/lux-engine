/**
 * @file LineListOperationHandlers.cpp
 * @brief FeatureFactory and server-side operation handler for LineListTransientFeature,
 *        plus client-side LineListProxy implementation.
 */

#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/renderer/features/gizmo/LineListOperation.hpp>
#include <lux/engine/render/renderer/features/gizmo/LineListTransientFeature.hpp>
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

    // ── Transient handler: replace all lines for this frame ─────────────

    void handleReplaceTransientLines(Ctx& ctx, const UploadLineListPayload& p)
    {
        auto* sc  = lookupScene(ctx.user_state, p.scene_id);
        auto* buf = sc ? sc->sceneRegistry().find<TransientLineListBuffer>() : nullptr;
        if (!buf) {
            replyToCurrent<UploadLineListPayload>(ctx,
                LineListUploadedReply{p.chunk_id, 1u});
            return;
        }

        auto blob = resolveBlob(ctx.program, p.vertex_data);
        assert(blob.size() == p.vertex_count * sizeof(GizmoVertex));

        buf->replace(
            reinterpret_cast<const GizmoVertex*>(blob.data()),
            p.vertex_count);

        replyToCurrent<UploadLineListPayload>(ctx,
            LineListUploadedReply{p.chunk_id, 0u});
    }

} // anonymous namespace

    // Typed-op: register/unregister generated from the op list. The Blob handler
    // above (handleReplaceTransientLines) is the only hand-written piece.
    using LineListOps = FeatureOpRegistrar<ServerOp<UploadLineListOp, &handleReplaceTransientLines>>;

    // =====================================================================
    //  LineListTransient factory
    // =====================================================================

    static FeatureHandle lineListTransientCreateFn(void* scene_ptr,
                                              const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        LineListTransientCommConfig cc{};
        if (param && param_size >= sizeof(LineListTransientCommConfig))
            cc = *static_cast<const LineListTransientCommConfig*>(param);

        LineListTransientFeature::Config cfg{};
        cfg.vertex_shader   = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.line_width      = cc.line_width;
        cfg.max_vertices    = cc.max_vertices;
        return sc->addFeature<LineListTransientFeature>(cfg);
    }

    const FeatureFactory kLineListTransientFactory{
        &lineListTransientCreateFn,
        &LineListOps::registerAll,
        &LineListOps::unregisterAll,
        "LineListTransient",
    };

// =====================================================================
//  LineListProxy — client-side proxy
// =====================================================================

RenderRequest<LineListUploadedReply> LineListProxy::uploadLines(
    RenderSceneId scene_id, uint32_t chunk_id,
    std::span<const GizmoVertex> vertices)
{
    UploadLineListPayload payload{};
    payload.scene_id     = scene_id;
    payload.chunk_id     = chunk_id;
    payload.vertex_count = static_cast<uint32_t>(vertices.size());
    return sendBlob<UploadLineListOp>(
        *session_, ops_, payload, std::as_bytes(vertices), alignof(GizmoVertex));
}

} // namespace lux::render
