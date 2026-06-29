/**
 * @file PCOperationHandlers.cpp
 * @brief FeatureFactory implementations for all 4 point cloud modes and
 *        server-side operation handlers for point cloud chunk upload/remove.
 */

#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudOperation.hpp>
#include <lux/engine/render/renderer/features/point_cloud/IPointCloudFeature.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureSimple.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureGPUDriven.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureLOD.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureSplatting.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureTransient.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PointCloudGpuData.hpp>
#include <lux/engine/render/resources/PointCloudResources.hpp>
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

    // ── Point cloud command handlers ────────────────────────────────────

    void handleUploadPointCloudChunk(Ctx& ctx, const UploadPointCloudChunkPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* pc_res = sc ? sc->sceneRegistry().find<PointCloudResources>() : nullptr;
        if (!pc_res || !pc_res->isInitialized()) {
            replyToCurrent<UploadPointCloudChunkPayload>(ctx,
                PointCloudChunkUploadedReply{p.chunk_id, 1u});
            return;
        }

        auto blob = resolveBlob(ctx.program, p.point_data);
        assert(blob.size() == p.point_count * sizeof(GpuPointVertex));
        assert((reinterpret_cast<std::uintptr_t>(blob.data()) % alignof(GpuPointVertex)) == 0);

        auto points = std::span<const GpuPointVertex>(
            reinterpret_cast<const GpuPointVertex*>(blob.data()),
            p.point_count);

        pc_res->queueUpload(p.chunk_id, points);
        replyToCurrent<UploadPointCloudChunkPayload>(ctx,
            PointCloudChunkUploadedReply{p.chunk_id, 0u});
    }

    void handleRemovePointCloudChunk(Ctx& ctx, const RemovePointCloudChunkPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* pc_res = sc ? sc->sceneRegistry().find<PointCloudResources>() : nullptr;
        if (!pc_res || !pc_res->isInitialized()) return;
        pc_res->globalBuffer().freeSlot(p.chunk_id);
        pc_res->nodeBuffer().removeNode(p.chunk_id);
    }

    void handleClearAllPointCloud(Ctx& ctx, const ClearAllPointCloudPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* pc_res = sc ? sc->sceneRegistry().find<PointCloudResources>() : nullptr;
        if (!pc_res || !pc_res->isInitialized())
        {
            replyToCurrent<ClearAllPointCloudPayload>(ctx, GenericOkReply{1u});
            return;
        }
        pc_res->queueClearAll();
        replyToCurrent<ClearAllPointCloudPayload>(ctx, GenericOkReply{0u});
    }

    void handleClearPointCloudChunk(Ctx& ctx, const ClearPointCloudChunkPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* pc_res = sc ? sc->sceneRegistry().find<PointCloudResources>() : nullptr;
        if (!pc_res || !pc_res->isInitialized())
        {
            replyToCurrent<ClearPointCloudChunkPayload>(ctx, GenericOkReply{1u});
            return;
        }
        pc_res->queueResetChunk(p.chunk_id);
        replyToCurrent<ClearPointCloudChunkPayload>(ctx, GenericOkReply{0u});
    }

    void handleSetPointCloudPointSize(Ctx& ctx, const SetPointCloudPointSizePayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        if (!sc) return;
        if (auto* f = sc->getFeatureAs<IPointCloudFeature>(p.feature))
            f->setPointSize(p.point_size);
    }

} // anonymous namespace

    // Typed-op: register/unregister generated from the op list. Shared by the 4
    // accumulating modes (Simple / GPUDriven / LOD / Splatting). The 5 handlers above are
    // the only hand-written pieces.
    using PCOps = FeatureOpRegistrar<
        ServerOp<UploadPointCloudChunkOp,  &handleUploadPointCloudChunk>,
        ServerOp<RemovePointCloudChunkOp,  &handleRemovePointCloudChunk>,
        ServerOp<ClearAllPointCloudOp,     &handleClearAllPointCloud>,
        ServerOp<ClearPointCloudChunkOp,   &handleClearPointCloudChunk>,
        ServerOp<SetPointCloudPointSizeOp, &handleSetPointCloudPointSize>>;

    // =====================================================================
    //  PCFeatureSimple factory
    // =====================================================================

    static FeatureHandle pcSimpleCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        PCSimpleCommConfig cc{};
        if (param && param_size >= sizeof(PCSimpleCommConfig))
            cc = *static_cast<const PCSimpleCommConfig*>(param);

        PCFeatureSimple::Config cfg{};
        cfg.vertex_shader      = cc.vertex_shader;
        cfg.fragment_shader    = cc.fragment_shader;
        cfg.initial_point_size = cc.initial_point_size;
        cfg.max_global_points  = cc.max_global_points;
        cfg.max_octree_nodes   = cc.max_octree_nodes;
        return sc->addFeature<PCFeatureSimple>(cfg);
    }

    const FeatureFactory kPCFeatureSimpleFactory{
        &pcSimpleCreateFn,
        &PCOps::registerAll,
        &PCOps::unregisterAll,
        "PCSimple",
    };

    // =====================================================================
    //  PCFeatureGPUDriven factory
    // =====================================================================

    static FeatureHandle pcGPUDrivenCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        PCGPUDrivenCommConfig cc{};
        if (param && param_size >= sizeof(PCGPUDrivenCommConfig))
            cc = *static_cast<const PCGPUDrivenCommConfig*>(param);

        PCFeatureGPUDriven::Config cfg{};
        cfg.compute_shader     = cc.compute_shader;
        cfg.vertex_shader      = cc.vertex_shader;
        cfg.fragment_shader    = cc.fragment_shader;
        cfg.initial_point_size = cc.initial_point_size;
        cfg.max_nodes          = cc.max_nodes;
        return sc->addFeature<PCFeatureGPUDriven>(cfg);
    }

    const FeatureFactory kPCFeatureGPUDrivenFactory{
        &pcGPUDrivenCreateFn,
        &PCOps::registerAll,
        &PCOps::unregisterAll,
        "PCGPUDriven",
    };

    // =====================================================================
    //  PCFeatureLOD factory
    // =====================================================================

    static FeatureHandle pcLODCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        PCLODCommConfig cc{};
        if (param && param_size >= sizeof(PCLODCommConfig))
            cc = *static_cast<const PCLODCommConfig*>(param);

        PCFeatureLOD::Config cfg{};
        cfg.compute_shader   = cc.compute_shader;
        cfg.vertex_shader    = cc.vertex_shader;
        cfg.fragment_shader  = cc.fragment_shader;
        cfg.point_size_world = cc.point_size_world;
        cfg.min_size         = cc.min_size;
        cfg.max_size         = cc.max_size;
        cfg.max_nodes        = cc.max_nodes;
        return sc->addFeature<PCFeatureLOD>(cfg);
    }

    const FeatureFactory kPCFeatureLODFactory{
        &pcLODCreateFn,
        &PCOps::registerAll,
        &PCOps::unregisterAll,
        "PCLOD",
    };

    // =====================================================================
    //  PCFeatureSplatting factory
    // =====================================================================

    static FeatureHandle pcSplattingCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        PCSplattingCommConfig cc{};
        if (param && param_size >= sizeof(PCSplattingCommConfig))
            cc = *static_cast<const PCSplattingCommConfig*>(param);

        PCFeatureSplatting::Config cfg{};
        cfg.compute_shader   = cc.compute_shader;
        cfg.vertex_shader    = cc.vertex_shader;
        cfg.fragment_shader  = cc.fragment_shader;
        cfg.point_size_world = cc.point_size_world;
        cfg.min_size         = cc.min_size;
        cfg.max_size         = cc.max_size;
        cfg.max_nodes        = cc.max_nodes;
        return sc->addFeature<PCFeatureSplatting>(cfg);
    }

    const FeatureFactory kPCFeatureSplattingFactory{
        &pcSplattingCreateFn,
        &PCOps::registerAll,
        &PCOps::unregisterAll,
        "PCSplatting",
    };

    // =====================================================================
    //  PCFeatureTransient factory
    // =====================================================================

namespace
{
    // ── Transient-specific handler: replace (not accumulate) ─────────────

    void handleReplaceTransientPoints(Ctx& ctx, const UploadPointCloudChunkPayload& p)
    {
        auto* sc  = lookupScene(ctx.user_state, p.scene_id);
        auto* buf = sc ? sc->sceneRegistry().find<TransientPointCloudBuffer>() : nullptr;
        if (!buf) {
            replyToCurrent<UploadPointCloudChunkPayload>(ctx,
                PointCloudChunkUploadedReply{p.chunk_id, 1u});
            return;
        }

        auto blob = resolveBlob(ctx.program, p.point_data);
        assert(blob.size() == p.point_count * sizeof(GpuPointVertex));

        buf->replace(
            reinterpret_cast<const GpuPointVertex*>(blob.data()),
            p.point_count);

        replyToCurrent<UploadPointCloudChunkPayload>(ctx,
            PointCloudChunkUploadedReply{p.chunk_id, 0u});
    }

} // anonymous namespace

    // Transient shares the 5-op layout but uses the REPLACE upload handler at slot 0; the
    // remove/clear handlers no-op for transient (no PointCloudResources), so they register
    // uniformly rather than as placeholder slots (a stray remove/clear just gets a graceful
    // error reply instead of a hung request).
    using PCTransientOps = FeatureOpRegistrar<
        ServerOp<UploadPointCloudChunkOp,  &handleReplaceTransientPoints>,
        ServerOp<RemovePointCloudChunkOp,  &handleRemovePointCloudChunk>,
        ServerOp<ClearAllPointCloudOp,     &handleClearAllPointCloud>,
        ServerOp<ClearPointCloudChunkOp,   &handleClearPointCloudChunk>,
        ServerOp<SetPointCloudPointSizeOp, &handleSetPointCloudPointSize>>;

    static FeatureHandle pcTransientCreateFn(void* scene_ptr,
                                        const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        PCTransientCommConfig cc{};
        if (param && param_size >= sizeof(PCTransientCommConfig))
            cc = *static_cast<const PCTransientCommConfig*>(param);

        PCFeatureTransient::Config cfg{};
        cfg.vertex_shader   = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.point_size      = cc.point_size;
        cfg.max_points      = cc.max_points;
        return sc->addFeature<PCFeatureTransient>(cfg);
    }

    const FeatureFactory kPCFeatureTransientFactory{
        &pcTransientCreateFn,
        &PCTransientOps::registerAll,
        &PCTransientOps::unregisterAll,
        "PCTransient",
    };

// =====================================================================
//  PointCloudProxy — client-side proxy
// =====================================================================

RenderRequest<PointCloudChunkUploadedReply> PointCloudProxy::uploadChunk(
    RenderSceneId scene_id, uint32_t chunk_id,
    std::span<const PointCloudPoint> points)
{
    UploadPointCloudChunkPayload payload{};
    payload.scene_id    = scene_id;
    payload.chunk_id    = chunk_id;
    payload.point_count = static_cast<uint32_t>(points.size());
    return sendBlob<UploadPointCloudChunkOp>(
        *session_, ops_, payload, std::as_bytes(points), alignof(PointCloudPoint));
}

void PointCloudProxy::removeChunk(RenderSceneId scene_id, uint32_t chunk_id)
{
    RemovePointCloudChunkPayload payload{};
    payload.scene_id = scene_id;
    payload.chunk_id = chunk_id;
    send<RemovePointCloudChunkOp>(*session_, ops_, payload);
}

RenderRequest<GenericOkReply> PointCloudProxy::clearAll(RenderSceneId scene_id)
{
    ClearAllPointCloudPayload payload{};
    payload.scene_id = scene_id;
    return sendWithReply<ClearAllPointCloudOp>(*session_, ops_, payload);
}

RenderRequest<GenericOkReply> PointCloudProxy::clearChunk(RenderSceneId scene_id, uint32_t chunk_id)
{
    ClearPointCloudChunkPayload payload{};
    payload.scene_id = scene_id;
    payload.chunk_id = chunk_id;
    return sendWithReply<ClearPointCloudChunkOp>(*session_, ops_, payload);
}

void PointCloudProxy::setPointSize(RenderSceneId scene_id, FeatureHandle feature, float point_size)
{
    SetPointCloudPointSizePayload payload{};
    payload.scene_id   = scene_id;
    payload.feature    = feature;
    payload.point_size = point_size;
    send<SetPointCloudPointSizeOp>(*session_, ops_, payload);
}

} // namespace lux::render
