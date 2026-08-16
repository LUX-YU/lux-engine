// ============================================================================
//  TrajectoryOperationHandlers.cpp — Trajectory 的手写残余:5 个 op 语义函数
//  (createFn/registrar/factory/Proxy 由 comm/genops/TrajectoryOperation.ops.cpp
//   生成并 extern 引用本文件的 handleTrajectory* —— 少定义即链接错误。)
// ============================================================================
#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/genops/TrajectoryOperation.ops.hpp>
#include <lux/engine/render/renderer/features/trajectory/TrajectoryLineFeature.hpp>
#include <lux/engine/render/resources/TrajectoryGpuData.hpp>
#include <lux/engine/render/resources/TrajectoryResources.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <cassert>
#include <span>

namespace lux::render
{
    // Defined in RenderServer.cpp
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        using Ctx = GeneralRenderServer::Dispatcher::Ctx;

        inline constexpr uint32_t kTrajectoryStatusOk             = 0u;
        inline constexpr uint32_t kTrajectoryStatusNotInitialized = 1u;
        inline constexpr uint32_t kTrajectoryStatusInvalidHandle  = 2u;

        TrajectoryResources* findTrajRes(Ctx& ctx, RenderSceneId scene_id)
        {
            auto* sc = lookupScene(ctx.user_state, scene_id);
            return sc ? sc->sceneRegistry().find<TrajectoryResources>() : nullptr;
        }

        std::span<const GpuTrajectoryVertex> decodePoints(Ctx& ctx, BlobRef blob_ref, uint32_t count)
        {
            auto blob = resolveBlob(ctx.program, blob_ref);
            assert(blob.size() == static_cast<std::size_t>(count) * sizeof(GpuTrajectoryVertex));
            assert((reinterpret_cast<std::uintptr_t>(blob.data()) % alignof(GpuTrajectoryVertex)) == 0);
            return { reinterpret_cast<const GpuTrajectoryVertex*>(blob.data()), count };
        }
    } // anonymous namespace

    void handleTrajectoryCreate(GeneralRenderServer::Dispatcher::Ctx& ctx,
                                const CreateTrajectoryPayload& p)
    {
        auto* traj_res = findTrajRes(ctx, p.scene_id);
        if (!traj_res || !traj_res->isInitialized()) {
            replyToCurrent<CreateTrajectoryPayload>(ctx,
                TrajectoryCreatedReply{TrajectoryHandle::invalid(), kTrajectoryStatusNotInitialized});
            return;
        }

        const TrajectoryHandle trajectory =
            traj_res->createTrajectory(decodePoints(ctx, p.point_data, p.point_count));
        if (!trajectory.isValid())
        {
            replyToCurrent<CreateTrajectoryPayload>(ctx,
                TrajectoryCreatedReply{TrajectoryHandle::invalid(), kTrajectoryStatusInvalidHandle});
            return;
        }

        replyToCurrent<CreateTrajectoryPayload>(ctx,
            TrajectoryCreatedReply{trajectory, kTrajectoryStatusOk});
    }

    void handleTrajectoryAppend(GeneralRenderServer::Dispatcher::Ctx& ctx,
                                const AppendTrajectoryPointsPayload& p)
    {
        auto* traj_res = findTrajRes(ctx, p.scene_id);
        if (!traj_res || !traj_res->isInitialized())
        {
            replyToCurrent<AppendTrajectoryPointsPayload>(ctx,
                GenericOkReply{kTrajectoryStatusNotInitialized});
            return;
        }

        const bool queued = traj_res->queueAppend(
            p.trajectory, decodePoints(ctx, p.point_data, p.point_count));
        replyToCurrent<AppendTrajectoryPointsPayload>(ctx,
            GenericOkReply{queued ? kTrajectoryStatusOk : kTrajectoryStatusInvalidHandle});
    }

    void handleTrajectoryClear(GeneralRenderServer::Dispatcher::Ctx& ctx,
                               const ClearTrajectoryPayload& p)
    {
        auto* traj_res = findTrajRes(ctx, p.scene_id);
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

    void handleTrajectoryRemove(GeneralRenderServer::Dispatcher::Ctx& ctx,
                                const RemoveTrajectoryPayload& p)
    {
        auto* traj_res = findTrajRes(ctx, p.scene_id);
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

    void handleTrajectoryReplace(GeneralRenderServer::Dispatcher::Ctx& ctx,
                                 const ReplaceTrajectoryPointsPayload& p)
    {
        auto* traj_res = findTrajRes(ctx, p.scene_id);
        if (!traj_res || !traj_res->isInitialized())
        {
            replyToCurrent<ReplaceTrajectoryPointsPayload>(ctx,
                GenericOkReply{kTrajectoryStatusNotInitialized});
            return;
        }

        const bool queued = traj_res->queueReplace(
            p.trajectory, decodePoints(ctx, p.point_data, p.point_count));
        replyToCurrent<ReplaceTrajectoryPointsPayload>(ctx,
            GenericOkReply{queued ? kTrajectoryStatusOk : kTrajectoryStatusInvalidHandle});
    }

} // namespace lux::render
