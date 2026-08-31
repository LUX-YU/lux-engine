/**
 * @file PCOperationHandlers.cpp
 * @brief FeatureFactory implementations for all 4 point cloud modes and
 *        server-side operation handlers for point cloud chunk upload/remove.
 */

#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>
#include <lux/engine/function/render/client/FeatureOpSend.hpp>
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp>
#include <lux/engine/function/render/client/RenderProgramSession.hpp>
#include <lux/engine/function/render/client/genops/PointCloudOperation.ops.hpp>
#include <lux/engine/render/renderer/features/point_cloud/IPointCloudFeature.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureSimple.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureGPUDriven.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureLOD.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureSplatting.hpp>
#include <lux/engine/render/renderer/features/point_cloud/PCFeatureTransient.hpp>
#include <lux/engine/render/resources/point_cloud/PointCloudGpuData.hpp>
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
        using Ctx = Dispatcher::Ctx;

        // ── Point cloud command handlers ────────────────────────────────────

        void handleUploadPointCloudChunk(Ctx& ctx, const UploadPointCloudChunkPayload& p)
        {
            auto* sc = lookupScene(ctx.user_state, p.scene_id);
            auto* pc_res = sc ? sc->resources().find<PointCloudResources>() : nullptr;
            if (!pc_res || !pc_res->isInitialized())
            {
                replyToCurrent<UploadPointCloudChunkPayload>(ctx, PointCloudChunkUploadedReply{p.chunk_id, 1u});
                return;
            }

            auto blob = resolveBlob(ctx.program, p.point_data);
            assert(blob.size() == p.point_count * sizeof(GpuPointVertex));
            assert((reinterpret_cast<std::uintptr_t>(blob.data()) % alignof(GpuPointVertex)) == 0);

            auto points =
                std::span<const GpuPointVertex>(reinterpret_cast<const GpuPointVertex*>(blob.data()), p.point_count);

            pc_res->queueUpload(p.chunk_id, points);
            replyToCurrent<UploadPointCloudChunkPayload>(ctx, PointCloudChunkUploadedReply{p.chunk_id, 0u});
        }

        void handleRemovePointCloudChunk(Ctx& ctx, const RemovePointCloudChunkPayload& p)
        {
            auto* sc = lookupScene(ctx.user_state, p.scene_id);
            auto* pc_res = sc ? sc->resources().find<PointCloudResources>() : nullptr;
            if (!pc_res || !pc_res->isInitialized())
                return;
            pc_res->globalBuffer().freeSlot(p.chunk_id);
            pc_res->nodeBuffer().removeNode(p.chunk_id);
        }

        void handleClearAllPointCloud(Ctx& ctx, const ClearAllPointCloudPayload& p)
        {
            auto* sc = lookupScene(ctx.user_state, p.scene_id);
            auto* pc_res = sc ? sc->resources().find<PointCloudResources>() : nullptr;
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
            auto* pc_res = sc ? sc->resources().find<PointCloudResources>() : nullptr;
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
            if (!sc)
                return;
            if (auto* f = sc->getFeatureAs<IPointCloudFeature>(p.feature))
                f->setPointSize(p.point_size);
        }

    } // anonymous namespace

    // Typed-op: register/unregister generated from the op list. Shared by the 4
    // accumulating modes (Simple / GPUDriven / LOD / Splatting). The 5 handlers above are
    // the only hand-written pieces.
    using PCOps = FeatureOpRegistrar<
        ServerOp<PointCloudUploadOp, &handleUploadPointCloudChunk>,
        ServerOp<PointCloudRemoveOp, &handleRemovePointCloudChunk>,
        ServerOp<PointCloudClearAllOp, &handleClearAllPointCloud>,
        ServerOp<PointCloudClearChunkOp, &handleClearPointCloudChunk>,
        ServerOp<PointCloudSetPointSizeOp, &handleSetPointCloudPointSize>>;

    // =====================================================================
    //  PCFeatureSimple factory
    // =====================================================================

    static Expected<FeatureHandle> pcSimpleCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<PCSimpleCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const PCSimpleCommConfig& cc = *decoded;

        PCFeatureSimple::Config cfg{};
        cfg.vertex_shader = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.initial_point_size = cc.initial_point_size;
        cfg.max_global_points = cc.max_global_points;
        cfg.max_octree_nodes = cc.max_octree_nodes;
        return sc->addFeature<PCFeatureSimple>(cfg);
    }

    static constexpr FeatureDescriptor kPCSimpleDescriptor{
        .type = featureId("lux.render.point_cloud_simple.v1"),
        .name = "PCSimple",
    };
    const FeatureFactory kPCFeatureSimpleFactory{
        &pcSimpleCreateFn,
        &PCOps::registerAll,
        &PCOps::unregisterAll,
        "PCSimple",
        -1,
        kPCSimpleDescriptor,
    };

    // =====================================================================
    //  PCFeatureGPUDriven factory
    // =====================================================================

    static Expected<FeatureHandle> pcGPUDrivenCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<PCGPUDrivenCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const PCGPUDrivenCommConfig& cc = *decoded;

        PCFeatureGPUDriven::Config cfg{};
        cfg.compute_shader = cc.compute_shader;
        cfg.vertex_shader = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.initial_point_size = cc.initial_point_size;
        cfg.max_nodes = cc.max_nodes;
        return sc->addFeature<PCFeatureGPUDriven>(cfg);
    }

    static constexpr FeatureDescriptor kPCGPUDrivenDescriptor{
        .type = featureId("lux.render.point_cloud_gpudriven.v1"),
        .name = "PCGPUDriven",
    };
    const FeatureFactory kPCFeatureGPUDrivenFactory{
        &pcGPUDrivenCreateFn,
        &PCOps::registerAll,
        &PCOps::unregisterAll,
        "PCGPUDriven",
        -1,
        kPCGPUDrivenDescriptor,
    };

    // =====================================================================
    //  PCFeatureLOD factory
    // =====================================================================

    static Expected<FeatureHandle> pcLODCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<PCLODCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const PCLODCommConfig& cc = *decoded;

        PCFeatureLOD::Config cfg{};
        cfg.compute_shader = cc.compute_shader;
        cfg.vertex_shader = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.point_size_world = cc.point_size_world;
        cfg.min_size = cc.min_size;
        cfg.max_size = cc.max_size;
        cfg.max_nodes = cc.max_nodes;
        return sc->addFeature<PCFeatureLOD>(cfg);
    }

    static constexpr FeatureDescriptor kPCLODDescriptor{
        .type = featureId("lux.render.point_cloud_lod.v1"),
        .name = "PCLOD",
    };
    const FeatureFactory kPCFeatureLODFactory{
        &pcLODCreateFn,
        &PCOps::registerAll,
        &PCOps::unregisterAll,
        "PCLOD",
        -1,
        kPCLODDescriptor,
    };

    // =====================================================================
    //  PCFeatureSplatting factory
    // =====================================================================

    static Expected<FeatureHandle> pcSplattingCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<PCSplattingCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const PCSplattingCommConfig& cc = *decoded;

        PCFeatureSplatting::Config cfg{};
        cfg.compute_shader = cc.compute_shader;
        cfg.vertex_shader = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.point_size_world = cc.point_size_world;
        cfg.min_size = cc.min_size;
        cfg.max_size = cc.max_size;
        cfg.max_nodes = cc.max_nodes;
        return sc->addFeature<PCFeatureSplatting>(cfg);
    }

    static constexpr FeatureDescriptor kPCSplattingDescriptor{
        .type = featureId("lux.render.point_cloud_splatting.v1"),
        .name = "PCSplatting",
    };
    const FeatureFactory kPCFeatureSplattingFactory{
        &pcSplattingCreateFn,
        &PCOps::registerAll,
        &PCOps::unregisterAll,
        "PCSplatting",
        -1,
        kPCSplattingDescriptor,
    };

    // =====================================================================
    //  PCFeatureTransient factory
    // =====================================================================

    namespace
    {
        // ── Transient-specific handler: replace (not accumulate) ─────────────

        void handleReplaceTransientPoints(Ctx& ctx, const UploadPointCloudChunkPayload& p)
        {
            auto* sc = lookupScene(ctx.user_state, p.scene_id);
            auto* buf = sc ? sc->resources().find<TransientPointCloudBuffer>() : nullptr;
            if (!buf)
            {
                replyToCurrent<UploadPointCloudChunkPayload>(ctx, PointCloudChunkUploadedReply{p.chunk_id, 1u});
                return;
            }

            auto blob = resolveBlob(ctx.program, p.point_data);
            assert(blob.size() == p.point_count * sizeof(GpuPointVertex));

            buf->replace(reinterpret_cast<const GpuPointVertex*>(blob.data()), p.point_count);

            replyToCurrent<UploadPointCloudChunkPayload>(ctx, PointCloudChunkUploadedReply{p.chunk_id, 0u});
        }

    } // anonymous namespace

    // Transient shares the 5-op layout but uses the REPLACE upload handler at slot 0; the
    // remove/clear handlers no-op for transient (no PointCloudResources), so they register
    // uniformly rather than as placeholder slots (a stray remove/clear just gets a graceful
    // error reply instead of a hung request).
    using PCTransientOps = FeatureOpRegistrar<
        ServerOp<PointCloudUploadOp, &handleReplaceTransientPoints>,
        ServerOp<PointCloudRemoveOp, &handleRemovePointCloudChunk>,
        ServerOp<PointCloudClearAllOp, &handleClearAllPointCloud>,
        ServerOp<PointCloudClearChunkOp, &handleClearPointCloudChunk>,
        ServerOp<PointCloudSetPointSizeOp, &handleSetPointCloudPointSize>>;

    static Expected<FeatureHandle> pcTransientCreateFn(void* scene_ptr, const void* param, size_t param_size)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);

        const auto decoded = decodeCommConfig<PCTransientCommConfig>(param, param_size);
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        const PCTransientCommConfig& cc = *decoded;

        PCFeatureTransient::Config cfg{};
        cfg.vertex_shader = cc.vertex_shader;
        cfg.fragment_shader = cc.fragment_shader;
        cfg.point_size = cc.point_size;
        cfg.max_points = cc.max_points;
        return sc->addFeature<PCFeatureTransient>(cfg);
    }

    static constexpr FeatureDescriptor kPCTransientDescriptor{
        .type = featureId("lux.render.point_cloud_transient.v1"),
        .name = "PCTransient",
    };
    const FeatureFactory kPCFeatureTransientFactory{
        &pcTransientCreateFn,
        &PCTransientOps::registerAll,
        &PCTransientOps::unregisterAll,
        "PCTransient",
        -1,
        kPCTransientDescriptor,
    };

} // namespace lux::render
