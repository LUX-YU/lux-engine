/**
 * @file TrajectoryOperationHandlers.cpp
 * @brief FeatureFactory implementations for all 3 trajectory modes and
 *        server-side operation handlers for trajectory create/append/clear/remove.
 */

#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/renderer/features/trajectory/TrajectoryOperation.hpp>
#include <lux/engine/render/renderer/features/trajectory/TrajectoryLineFeature.hpp>
#include <lux/engine/render/renderer/features/trajectory/TrajectoryGpuData.hpp>
#include <lux/engine/render/resources/TrajectoryResources.hpp>
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

    inline constexpr uint32_t kTrajectoryStatusOk            = 0u;
    inline constexpr uint32_t kTrajectoryStatusNotInitialized = 1u;
    inline constexpr uint32_t kTrajectoryStatusInvalidHandle  = 2u;

    // ── Trajectory command handlers ─────────────────────────────────────

    void handleCreateTrajectory(Ctx& ctx, const CreateTrajectoryPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* traj_res = sc ? sc->sceneRegistry().find<TrajectoryResources>() : nullptr;
        if (!traj_res || !traj_res->isInitialized()) {
            replyToCurrent<CreateTrajectoryPayload>(ctx,
                TrajectoryCreatedReply{TrajectoryHandle::invalid(), kTrajectoryStatusNotInitialized});
            return;
        }

        auto blob = resolveBlob(ctx.program, p.point_data);
        assert(blob.size() == static_cast<std::size_t>(p.point_count) * sizeof(GpuTrajectoryVertex));
        assert((reinterpret_cast<std::uintptr_t>(blob.data()) % alignof(GpuTrajectoryVertex)) == 0);
        auto points = std::span<const GpuTrajectoryVertex>(
            reinterpret_cast<const GpuTrajectoryVertex*>(blob.data()),
            p.point_count);

        const TrajectoryHandle trajectory = traj_res->createTrajectory(points);
        if (!trajectory.valid())
        {
            replyToCurrent<CreateTrajectoryPayload>(ctx,
                TrajectoryCreatedReply{TrajectoryHandle::invalid(), kTrajectoryStatusInvalidHandle});
            return;
        }

        replyToCurrent<CreateTrajectoryPayload>(ctx,
            TrajectoryCreatedReply{trajectory, kTrajectoryStatusOk});
    }

    void handleAppendTrajectoryPoints(Ctx& ctx, const AppendTrajectoryPointsPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* traj_res = sc ? sc->sceneRegistry().find<TrajectoryResources>() : nullptr;
        if (!traj_res || !traj_res->isInitialized())
        {
            replyToCurrent<AppendTrajectoryPointsPayload>(ctx,
                GenericOkReply{kTrajectoryStatusNotInitialized});
            return;
        }

        auto blob = resolveBlob(ctx.program, p.point_data);
        assert(blob.size() == static_cast<std::size_t>(p.point_count) * sizeof(GpuTrajectoryVertex));
        assert((reinterpret_cast<std::uintptr_t>(blob.data()) % alignof(GpuTrajectoryVertex)) == 0);
        auto points = std::span<const GpuTrajectoryVertex>(
            reinterpret_cast<const GpuTrajectoryVertex*>(blob.data()),
            p.point_count);

        const bool queued = traj_res->queueAppend(p.trajectory, points);
        replyToCurrent<AppendTrajectoryPointsPayload>(ctx,
            GenericOkReply{queued ? kTrajectoryStatusOk : kTrajectoryStatusInvalidHandle});
    }

    void handleClearTrajectory(Ctx& ctx, const ClearTrajectoryPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* traj_res = sc ? sc->sceneRegistry().find<TrajectoryResources>() : nullptr;
        if (!traj_res || !traj_res->isInitialized())
        {
            replyToCurrent<ClearTrajectoryPayload>(ctx,
                GenericOkReply{kTrajectoryStatusNotInitialized});
            return;
        }

        const bool queued = traj_res->queueClear(p.trajectory);
        replyToCurrent<ClearTrajectoryPayload>(ctx,
            GenericOkReply{queued ? kTrajectoryStatusOk : kTrajectoryStatusInvalidHandle});
    }

    void handleRemoveTrajectory(Ctx& ctx, const RemoveTrajectoryPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* traj_res = sc ? sc->sceneRegistry().find<TrajectoryResources>() : nullptr;
        if (!traj_res || !traj_res->isInitialized())
        {
            replyToCurrent<RemoveTrajectoryPayload>(ctx,
                GenericOkReply{kTrajectoryStatusNotInitialized});
            return;
        }

        const bool queued = traj_res->queueRemove(p.trajectory);
        replyToCurrent<RemoveTrajectoryPayload>(ctx,
            GenericOkReply{queued ? kTrajectoryStatusOk : kTrajectoryStatusInvalidHandle});
    }

    void handleReplaceTrajectoryPoints(Ctx& ctx, const ReplaceTrajectoryPointsPayload& p)
    {
        auto* sc = lookupScene(ctx.user_state, p.scene_id);
        auto* traj_res = sc ? sc->sceneRegistry().find<TrajectoryResources>() : nullptr;
        if (!traj_res || !traj_res->isInitialized())
        {
            replyToCurrent<ReplaceTrajectoryPointsPayload>(ctx,
                GenericOkReply{kTrajectoryStatusNotInitialized});
            return;
        }

        auto blob = resolveBlob(ctx.program, p.point_data);
        assert(blob.size() == static_cast<std::size_t>(p.point_count) * sizeof(GpuTrajectoryVertex));
        assert((reinterpret_cast<std::uintptr_t>(blob.data()) % alignof(GpuTrajectoryVertex)) == 0);
        auto points = std::span<const GpuTrajectoryVertex>(
            reinterpret_cast<const GpuTrajectoryVertex*>(blob.data()),
            p.point_count);

        const bool queued = traj_res->queueReplace(p.trajectory, points);
        replyToCurrent<ReplaceTrajectoryPointsPayload>(ctx,
            GenericOkReply{queued ? kTrajectoryStatusOk : kTrajectoryStatusInvalidHandle});
    }

} // anonymous namespace

    // Typed-op: register/unregister generated from the op list (kind + opcode override
    // pick the ring). The 5 handlers above are the only hand-written pieces; shared by
    // all 3 trajectory modes.
    using TrajectoryOps = FeatureOpRegistrar<
        ServerOp<CreateTrajectoryOp,        &handleCreateTrajectory>,
        ServerOp<AppendTrajectoryPointsOp,  &handleAppendTrajectoryPoints>,
        ServerOp<ClearTrajectoryOp,         &handleClearTrajectory>,
        ServerOp<RemoveTrajectoryOp,        &handleRemoveTrajectory>,
        ServerOp<ReplaceTrajectoryPointsOp, &handleReplaceTrajectoryPoints>>;

// =====================================================================
//  TrajectoryLineFeature factory
// =====================================================================

static FeatureHandle trajLineCreateFn(void* scene_ptr, const void* param, size_t param_size)
{
    auto* sc = static_cast<RenderScene*>(scene_ptr);

    TrajectoryLineCommConfig cc{};
    if (param && param_size >= sizeof(TrajectoryLineCommConfig))
        cc = *static_cast<const TrajectoryLineCommConfig*>(param);

    TrajectoryLineFeature::Config cfg{};
    cfg.vertex_shader        = cc.vertex_shader;
    cfg.fragment_shader      = cc.fragment_shader;
    cfg.max_global_vertices  = cc.max_global_vertices;
    return sc->addFeature<TrajectoryLineFeature>(cfg);
}

const FeatureFactory kTrajectoryLineFactory{
    &trajLineCreateFn,
    &TrajectoryOps::registerAll,
    &TrajectoryOps::unregisterAll,
    "TrajectoryLine",
};

// =====================================================================
//  TrajectoryProxy — client-side proxy
// =====================================================================

RenderRequest<TrajectoryCreatedReply> TrajectoryProxy::newTrajectory(
    RenderSceneId scene_id,
    std::span<const TrajectoryPoint> points)
{
    CreateTrajectoryPayload payload{};
    payload.scene_id    = scene_id;
    payload.point_count = static_cast<uint32_t>(points.size());
    return sendBlob<CreateTrajectoryOp>(
        *session_, ops_, payload, std::as_bytes(points), alignof(TrajectoryPoint));
}

RenderRequest<GenericOkReply> TrajectoryProxy::appendPoints(
    RenderSceneId scene_id, TrajectoryHandle trajectory,
    std::span<const TrajectoryPoint> points)
{
    AppendTrajectoryPointsPayload payload{};
    payload.scene_id    = scene_id;
    payload.trajectory  = trajectory;
    payload.point_count = static_cast<uint32_t>(points.size());
    return sendBlob<AppendTrajectoryPointsOp>(
        *session_, ops_, payload, std::as_bytes(points), alignof(TrajectoryPoint));
}

RenderRequest<GenericOkReply> TrajectoryProxy::clear(RenderSceneId scene_id, TrajectoryHandle trajectory)
{
    ClearTrajectoryPayload payload{};
    payload.scene_id   = scene_id;
    payload.trajectory = trajectory;
    return sendWithReply<ClearTrajectoryOp>(*session_, ops_, payload);
}

RenderRequest<GenericOkReply> TrajectoryProxy::remove(RenderSceneId scene_id, TrajectoryHandle trajectory)
{
    RemoveTrajectoryPayload payload{};
    payload.scene_id   = scene_id;
    payload.trajectory = trajectory;
    return sendWithReply<RemoveTrajectoryOp>(*session_, ops_, payload);
}

RenderRequest<GenericOkReply> TrajectoryProxy::replacePoints(
    RenderSceneId scene_id, TrajectoryHandle trajectory,
    std::span<const TrajectoryPoint> points)
{
    ReplaceTrajectoryPointsPayload payload{};
    payload.scene_id    = scene_id;
    payload.trajectory  = trajectory;
    payload.point_count = static_cast<uint32_t>(points.size());
    return sendBlob<ReplaceTrajectoryPointsOp>(
        *session_, ops_, payload, std::as_bytes(points), alignof(TrajectoryPoint));
}

} // namespace lux::render
