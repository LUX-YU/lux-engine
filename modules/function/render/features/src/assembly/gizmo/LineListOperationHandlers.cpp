// ============================================================================
//  LineListOperationHandlers.cpp — LineList 的手写残余:op 语义函数
//  (createFn/registrar/factory/Proxy 由 comm/genops/LineListOperation.ops.cpp
//   生成并 extern 引用本函数。)
// ============================================================================
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/genops/LineListOperation.ops.hpp>
#include <lux/engine/render/renderer/features/gizmo/LineListTransientFeature.hpp>
#include <lux/engine/function/render/client/features/gizmo/GizmoVertex.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <cassert>

namespace lux::render
{
    // Defined in RenderServer.cpp
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    // ── 瞬态语义:整帧替换本场景的全部线段(chunk_id 现阶段忽略,
    //    last-writer-wins),blob 解码后回执上传状态。──
    void handleLineListUpload(GeneralRenderServer::Dispatcher::Ctx& ctx, const UploadLineListPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* buf = sc ? sc->resources().find<TransientLineListBuffer>() : nullptr;
        if (!buf)
        {
            replyToCurrent<UploadLineListPayload>(ctx, LineListUploadedReply{p.chunk_id, 1u});
            return;
        }

        auto blob = resolveBlob(ctx.program, p.vertex_data);
        assert(blob.size() == p.vertex_count * sizeof(GizmoVertex));

        buf->replace(reinterpret_cast<const GizmoVertex*>(blob.data()), p.vertex_count);

        replyToCurrent<UploadLineListPayload>(ctx, LineListUploadedReply{p.chunk_id, 0u});
    }

} // namespace lux::render
