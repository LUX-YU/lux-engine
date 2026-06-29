// ============================================================================
//  SkinningOperationHandlers.cpp — SkinningFeature factory + feature-scoped
//  bone-upload commands. The per-instance skinning logic + its two command
//  handlers live HERE (a feature), not in the core RenderServer dispatcher, and
//  are registered with DYNAMIC TypeIds via register_ops_fn (the grid pattern).
//  The core protocol no longer names skinning.
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>   // Dispatcher, Ctx, lookupScene, resolveBlob
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>   // FeatureOpRegistrar / ServerOp
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>  // send / sendBlob
#include <lux/engine/render/comm/RenderProtocol.hpp>          // opcodes
#include <lux/engine/render/renderer/features/skinning/SkinningOperation.hpp>
#include <lux/engine/render/renderer/features/skinning/SkinningFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/RendererContext.hpp>              // RenderContext::globalRegistry
#include <lux/engine/render/comm/client/RenderSession.hpp>    // SkinningProxy::builder()

#include <lux/engine/render/resources/vertex/SkinningResources.hpp>   // SkinningResources, BoneMatrixGpu
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>     // InstanceResources, InstanceSlot
#include <lux/engine/render/resources/mesh/MeshResources.hpp>         // MeshResources, handle_cast<MeshHandle>
#include <lux/engine/render/resources/vertex/MeshInputPool.hpp>       // MeshInputPool

#include <cstdint>
#include <iostream>
#include <span>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    // Exported by the server for feature operation handlers (resolves a scene
    // from the dispatcher user_state). Forward-declared here — the grid-handler
    // convention — to avoid pulling the heavy RenderServerImpl.hpp into a feature.
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        // Trivial index/gen handle reinterpret (RMeshHandle → MeshHandle). Mirrors
        // RenderServerImpl.hpp's handle_cast; kept local so this feature stays
        // decoupled from the server impl header.
        template <typename To, typename From>
        To handle_cast(From h) noexcept { return To{h.index, h.gen}; }

        // GPU skinning for one instance: resolve the mesh's input vertex range,
        // upload its bone palette + queue a skinning dispatch, and point the
        // instance's property at the skinned output. Pure on its parameters +
        // public resource API — no server internals (that is what lets it live in
        // a feature). Moved verbatim from the old RenderServer dispatcher.
        void applyBoneSkinningOne(SkinningResources* skin,
                                  InstanceResources* inst,
                                  MeshResources* mesh_res,
                                  MeshInputPool* mip,
                                  RenderObjectHandle object,
                                  RMeshHandle mesh,
                                  const BoneMatrixGpu* mats,
                                  uint32_t bone_count)
        {
            if (mats == nullptr || bone_count == 0) return;

            auto* gpu = mesh_res->getGpuRecord(handle_cast<MeshHandle>(mesh));
            if (!gpu || gpu->vertex_stride == 0 ||
                !gpu->ready.load(std::memory_order_acquire))
                return;
            const uint32_t in_base =
                static_cast<uint32_t>(gpu->vertex_buffer_range.offset / gpu->vertex_stride);
            const uint32_t vcount =
                static_cast<uint32_t>(gpu->vertex_buffer_range.size / gpu->vertex_stride);
            if (vcount == 0) return;

            const uint32_t in_pool_id = mip ? mip->ensureRegistered() : ~0u;
            if (in_pool_id == ~0u) {
                static uint32_t warn_n = 0;
                if ((++warn_n & (warn_n - 1)) == 0)
                    std::cerr << "[Skinning] input vertex pool unavailable; skinned instance"
                                 " skipped (x" << warn_n << ")\n";
                return;
            }

            const uint32_t palette_base = skin->uploadBonePalette(mats, bone_count);
            if (palette_base == ~0u) {
                static uint32_t warn_n = 0;
                if ((++warn_n & (warn_n - 1)) == 0)
                    std::cerr << "[Skinning] bone palette full this frame (bone_count="
                              << bone_count << "); skinned instance skipped (x" << warn_n << ")\n";
                return;
            }
            const VertexSourceHandle out =
                skin->queueDispatch(in_pool_id, in_base, vcount, palette_base, bone_count);
            if (!out.valid()) {
                static uint32_t warn_n = 0;
                if ((++warn_n & (warn_n - 1)) == 0)
                    std::cerr << "[Skinning] output vertex pool exhausted (vcount="
                              << vcount << "); skinned instance skipped (x" << warn_n << ")\n";
                return;
            }

            const InstanceSlot slot = inst->resolveSlot(object);
            if (!inst->isAlive(object)) return;
            auto& prop = inst->propertyAt(slot);
            prop.vertex_pool_id      = out.pool_id;
            prop.vertex_base         = out.vertex_base;
            prop.vertex_count        = vcount;
            prop.input_vertex_offset = in_base;
            inst->markPropertyDirty(slot);
        }

        // Resolve the per-scene + global resources a bone-upload handler needs,
        // via PUBLIC API only (lookupScene → scene registry + renderContext).
        struct SkinDeps { SkinningResources* skin; InstanceResources* inst;
                          MeshResources* mesh_res; MeshInputPool* mip; RenderScene* sc; };
        SkinDeps resolveSkinDeps(Ctx& ctx, RenderSceneId scene_id)
        {
            SkinDeps d{};
            d.sc = lookupScene(ctx.user_state, scene_id);
            if (!d.sc) return d;
            d.skin     = d.sc->sceneRegistry().find<SkinningResources>();
            d.inst     = d.sc->sceneRegistry().find<InstanceResources>();
            d.mesh_res = d.sc->renderContext().globalRegistry().find<MeshResources>();
            d.mip      = d.sc->sceneRegistry().find<MeshInputPool>();
            return d;
        }

        void handleUploadBonePalette(Ctx& ctx, const UploadBonePalettePayload& p)
        {
            auto d = resolveSkinDeps(ctx, p.scene_id);
            if (!d.skin || !d.inst || !d.mesh_res || !d.skin->initialized()) return;
            d.skin->beginFrameIfNew(d.sc->frameSerial());
            auto bone_bytes = resolveBlob(ctx.program, p.bones);
            const auto* mats = reinterpret_cast<const BoneMatrixGpu*>(bone_bytes.data());
            applyBoneSkinningOne(d.skin, d.inst, d.mesh_res, d.mip, p.object, p.mesh, mats, p.bone_count);
        }

        void handleUploadBoneBatch(Ctx& ctx, const UploadBoneBatchPayload& p)
        {
            if (p.entry_count == 0) return;
            auto d = resolveSkinDeps(ctx, p.scene_id);
            if (!d.skin || !d.inst || !d.mesh_res || !d.skin->initialized()) return;
            d.skin->beginFrameIfNew(d.sc->frameSerial());

            auto entry_bytes = resolveBlob(ctx.program, p.entries);
            auto bone_bytes  = resolveBlob(ctx.program, p.bones);
            if (entry_bytes.size() < static_cast<size_t>(p.entry_count) * sizeof(BoneBatchEntry))
                return;  // truncated payload — drop the whole batch
            const auto* entries  = reinterpret_cast<const BoneBatchEntry*>(entry_bytes.data());
            const auto* all_mats = reinterpret_cast<const BoneMatrixGpu*>(bone_bytes.data());
            const uint32_t total_bones =
                static_cast<uint32_t>(bone_bytes.size() / sizeof(BoneMatrixGpu));

            for (uint32_t i = 0; i < p.entry_count; ++i)
            {
                const BoneBatchEntry& e = entries[i];
                if (static_cast<uint64_t>(e.bone_offset) + e.bone_count > total_bones)
                    continue;  // out-of-range slice — skip this instance
                applyBoneSkinningOne(d.skin, d.inst, d.mesh_res, d.mip, e.object, e.mesh,
                                     all_mats + e.bone_offset, e.bone_count);
            }
        }
    } // namespace

    // ── Uniform factory interface ────────────────────────────────────────────
    static FeatureHandle skinningCreateFn(void* scene_ptr, const void* /*param*/, size_t /*sz*/)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        SkinningFeature::Config cfg{};   // compute shader self-loads from builtin
        return sc->addFeature<SkinningFeature>(cfg);
    }

    // Typed-op: register/unregister generated from the op list. The 2 handlers above are
    // the only hand-written pieces.
    using SkinningOps = FeatureOpRegistrar<
        ServerOp<UploadBonePaletteOp, &handleUploadBonePalette>,
        ServerOp<UploadBoneBatchOp,   &handleUploadBoneBatch>>;

    const FeatureFactory kSkinningFeatureFactory{
        &skinningCreateFn,
        &SkinningOps::registerAll,
        &SkinningOps::unregisterAll,
        "Skinning",
    };

    // ── Client-side proxy (sends bone uploads via the dynamic op-ids) ─────────
    void SkinningProxy::uploadBonePalette(RenderSceneId scene_id, RenderObjectHandle object,
                                          RMeshHandle mesh, const void* bone_matrices,
                                          uint32_t bone_count)
    {
        if (bone_count == 0 || bone_matrices == nullptr
            || ops_.id<UploadBonePaletteOp>() == kInvalidTypeId) return;
        // Single blob → the Blob-kind sugar fills payload.bones + sends (no reply).
        UploadBonePalettePayload p{};
        p.scene_id   = scene_id;
        p.object     = object;
        p.mesh       = mesh;
        p.bone_count = bone_count;
        sendBlob<UploadBonePaletteOp>(*session_, ops_, p,
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(bone_matrices), bone_count * 64u },
            16u);
    }

    void SkinningProxy::uploadBoneBatch(RenderSceneId scene_id, std::span<const BoneBatchEntry> entries,
                                        const void* bone_matrices, uint32_t bone_total_count)
    {
        if (entries.empty() || bone_matrices == nullptr || bone_total_count == 0
            || ops_.id<UploadBoneBatchOp>() == kInvalidTypeId) return;
        // Two blobs → fill both BlobRefs by hand, then send as a plain Stream op.
        auto& b = session_->builder();
        UploadBoneBatchPayload p{};
        p.scene_id    = scene_id;
        p.entry_count = static_cast<uint32_t>(entries.size());
        p.entries     = b.pushBlob(std::as_bytes(entries), alignof(BoneBatchEntry));
        p.bones       = b.pushBlob(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(bone_matrices), bone_total_count * 64u },
            16u);
        send<UploadBoneBatchOp>(*session_, ops_, p);
    }

} // namespace lux::render
