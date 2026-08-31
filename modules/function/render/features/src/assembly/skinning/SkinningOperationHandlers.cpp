// ============================================================================
//  SkinningOperationHandlers.cpp — SkinningFeature factory + feature-scoped
//  bone-upload commands. The per-instance skinning logic + its two command
//  handlers live HERE (a feature), not in the core RenderServer dispatcher, and
//  are registered with DYNAMIC TypeIds via register_ops_fn (the grid pattern).
//  The core protocol no longer names skinning.
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>                // Dispatcher, Ctx, lookupScene, resolveBlob
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>          // FeatureOpRegistrar / ServerOp
#include <lux/engine/function/render/client/FeatureOpSend.hpp>           // send / sendBlob
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp> // FeatureFactory / GenericOkReply
#include <lux/engine/function/render/client/features/skinning/SkinningOperation.hpp>
#include <lux/engine/render/renderer/features/skinning/SkinningFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>                  // RenderContext::globalRegistry
#include <lux/engine/function/render/client/RenderProgramSession.hpp> // SkinningProxy::builder()

#include <lux/engine/render/resources/vertex/SkinningResources.hpp> // SkinningResources, BoneMatrixGpu
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>   // InstanceResources, InstanceSlot
#include <lux/engine/render/resources/mesh/MeshResources.hpp>       // MeshResources, handle_cast<MeshHandle>
#include <lux/engine/render/resources/vertex/StaticVertexPoolSet.hpp>

#include <cstdint>
#include <span>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx = Dispatcher::Ctx;

    // Exported by the server for feature operation handlers (resolves a scene
    // from the dispatcher user_state). Forward-declared here — the grid-handler
    // convention — to avoid pulling the heavy RenderServerImpl.hpp into a feature.
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        //(本地那份 handle_cast 副本已删 —— 它当初存在的理由是"躲开服务端
        // Impl 头",而正版 handle_cast 现已归位到 L0 的 core/RenderResourceHandle.hpp,
        // 谁都可以正大光明地用。**复制一份来躲开一个不该 include 的头,是症状
        // 不是解法** —— 症状消失的正确方式是把那个类型工具放对地方。)

        // Resolve the per-scene + global resources a bone-upload handler needs,
        // via PUBLIC API only (lookupScene → scene registry + renderContext).
        struct SkinDeps
        {
            SkinningResources* skin;
            InstanceResources* inst;
            MeshResources* mesh_res;
            StaticVertexPoolSet* pools;
            RenderScene* sc;
        };

        // GPU skinning for one instance: resolve the mesh's input vertex range,
        // upload its bone palette + queue a skinning dispatch, and point the
        // instance's property at the skinned output. Public resource API only —
        // no server internals (that is what lets it live in a feature).
        //
        // 三处「资源不够,跳过这个实例」不返回错误,而是走自发上报:一帧已经在飞了,
        // 没有调用方能处置。此前每处各写一份幂次限流防刷屏 —— 现在合并计数是通道的
        // 职责(同一批内同键累加 occurrences),失败点只负责说清发生了什么。
        void applyBoneSkinningOne(
            const SkinDeps& d,
            RenderObjectHandle object,
            RMeshHandle mesh,
            const BoneMatrixGpu* mats,
            uint32_t bone_count
        )
        {
            auto* skin = d.skin;
            auto* inst = d.inst;
            auto* mesh_res = d.mesh_res;
            auto* pools = d.pools;
            auto& ctx = d.sc->renderContext();
            const auto scene_index = d.sc->sceneId().index;
            const auto frame_serial = d.sc->frameSerial();

            if (mats == nullptr || bone_count == 0)
                return;

            auto* gpu = mesh_res->getGpuRecord(handle_cast<MeshHandle>(mesh));
            if (!gpu || gpu->vertex_stride == 0 || !gpu->ready)
                return;
            const uint32_t in_base = static_cast<uint32_t>(gpu->vertex_buffer_range.offset / gpu->vertex_stride);
            const uint32_t vcount = static_cast<uint32_t>(gpu->vertex_buffer_range.size / gpu->vertex_stride);
            if (vcount == 0)
                return;

            const uint32_t in_pool_id = pools ? pools->ensureRegistered(gpu->vbo_segment, gpu->layout_id) : ~0u;
            if (in_pool_id == ~0u)
            {
                ctx.reportError(renderError<err::frame::SkinningInputPoolUnavailable>(), scene_index, frame_serial);
                return;
            }

            const uint32_t palette_base = skin->uploadBonePalette(mats, bone_count);
            if (palette_base == ~0u)
            {
                ctx.reportError(renderError<err::frame::BonePaletteFull>(bone_count), scene_index, frame_serial);
                return;
            }

            const VertexSourceHandle out = skin->queueDispatch(in_pool_id, in_base, vcount, palette_base, bone_count);
            if (!out.valid())
            {
                ctx.reportError(renderError<err::frame::SkinnedVertexPoolExhausted>(vcount), scene_index, frame_serial);
                return;
            }

            const InstanceSlot slot = inst->resolveSlot(object);
            if (!inst->isAlive(object))
                return;
            auto& prop = inst->propertyAt(slot);
            prop.vertex_pool_id = out.pool_id;
            prop.vertex_base = out.vertex_base;
            prop.vertex_count = vcount;
            prop.input_vertex_offset = in_base;
            inst->markPropertyDirty(slot);
        }

        SkinDeps resolveSkinDeps(Ctx& ctx, RenderSceneId scene_id)
        {
            SkinDeps d{};
            d.sc = lookupScene(ctx.user_state, scene_id);
            if (!d.sc)
                return d;
            d.skin = d.sc->sceneRegistry().find<SkinningResources>();
            d.inst = d.sc->sceneRegistry().find<InstanceResources>();
            d.mesh_res = d.sc->renderContext().globalRegistry().find<MeshResources>();
            d.pools = d.sc->sceneRegistry().find<StaticVertexPoolSet>();
            return d;
        }
    } // anonymous namespace (helpers)

    void handleUploadBonePalette(GeneralRenderServer::Dispatcher::Ctx& ctx, const UploadBonePalettePayload& p)
    {
        auto d = resolveSkinDeps(ctx, p.scene_id);
        const bool is_missing_skin = d.skin == nullptr;
        const bool is_missing_instances = d.inst == nullptr;
        const bool is_missing_meshes = d.mesh_res == nullptr;
        const bool is_uninitialized_skin = !is_missing_skin && !d.skin->initialized();
        const bool is_invalid_dependencies = is_missing_skin || is_missing_instances || is_missing_meshes ||
            is_uninitialized_skin;
        if (is_invalid_dependencies)
            return;
        d.skin->beginFrameIfNew(d.sc->frameSerial());
        auto bone_bytes = resolveBlob(ctx.program, p.bones);
        const auto* mats = reinterpret_cast<const BoneMatrixGpu*>(bone_bytes.data());
        applyBoneSkinningOne(d, p.object, p.mesh, mats, p.bone_count);
    }

    void handleUploadBoneBatch(GeneralRenderServer::Dispatcher::Ctx& ctx, const UploadBoneBatchPayload& p)
    {
        if (p.entry_count == 0)
            return;
        auto d = resolveSkinDeps(ctx, p.scene_id);
        const bool is_missing_skin = d.skin == nullptr;
        const bool is_missing_instances = d.inst == nullptr;
        const bool is_missing_meshes = d.mesh_res == nullptr;
        const bool is_uninitialized_skin = !is_missing_skin && !d.skin->initialized();
        const bool is_invalid_dependencies = is_missing_skin || is_missing_instances || is_missing_meshes ||
            is_uninitialized_skin;
        if (is_invalid_dependencies)
            return;
        d.skin->beginFrameIfNew(d.sc->frameSerial());

        auto entry_bytes = resolveBlob(ctx.program, p.entries);
        auto bone_bytes = resolveBlob(ctx.program, p.bones);
        if (entry_bytes.size() < static_cast<size_t>(p.entry_count) * sizeof(BoneBatchEntry))
            return; // truncated payload — drop the whole batch
        const auto* entries = reinterpret_cast<const BoneBatchEntry*>(entry_bytes.data());
        const auto* all_mats = reinterpret_cast<const BoneMatrixGpu*>(bone_bytes.data());
        const uint32_t total_bones = static_cast<uint32_t>(bone_bytes.size() / sizeof(BoneMatrixGpu));

        for (uint32_t i = 0; i < p.entry_count; ++i)
        {
            const BoneBatchEntry& e = entries[i];
            if (static_cast<uint64_t>(e.bone_offset) + e.bone_count > total_bones)
                continue; // out-of-range slice — skip this instance
            applyBoneSkinningOne(d, e.object, e.mesh, all_mats + e.bone_offset, e.bone_count);
        }
    }

} // namespace lux::render
