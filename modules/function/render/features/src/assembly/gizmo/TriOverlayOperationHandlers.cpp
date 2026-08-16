// ============================================================================
//  TriOverlayOperationHandlers.cpp — TriOverlay 的手写残余:op 语义函数
//  (createFn/registrar/factory/Proxy 由 comm/genops/TriOverlayOperation.ops.cpp
//   生成并 extern 引用本函数。)
// ============================================================================
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/genops/TriOverlayOperation.ops.hpp>
#include <lux/engine/render/renderer/features/gizmo/TriOverlayTransientFeature.hpp>
#include <lux/engine/function/render/client/features/gizmo/GizmoVertex.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <cassert>

namespace lux::render
{
    // Defined in RenderServer.cpp
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    // ── 瞬态语义:整帧替换本场景的全部覆盖三角(last-writer-wins),
    //    blob 解码后回执上传状态。──
    void handleTriOverlayUpload(GeneralRenderServer::Dispatcher::Ctx& ctx,
                                const UploadTriOverlayPayload& p)
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

} // namespace lux::render
