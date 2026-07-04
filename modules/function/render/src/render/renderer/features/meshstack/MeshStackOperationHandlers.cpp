// ============================================================================
//  MeshStackOperationHandlers.cpp — StandardMeshStackFeature factory + the
//  feature-scoped mesh-instance commands. The add / remove / per-view
//  visibility / flags / render-state / user-meta / transform-batch handlers
//  live HERE (a feature), not in the core RenderServer dispatcher, and are
//  registered with DYNAMIC TypeIds via register_ops_fn (the grid / light
//  pattern). AddMeshInstance REPLIES with the new RenderObjectHandle
//  (replyToCurrent, request-id-correlated). The core protocol no longer names
//  mesh instances.
//
//  The heavy mesh ASSEMBLY (instance sections / cull meta / vertex-pool resolution,
//  the global mesh arena, and async mesh-data upload) lives HERE now (Stage C) — it is
//  internal to this TU; the core RenderServer no longer hosts any mesh body. The
//  assembly reaches the server's Vulkan stack through GeneralRenderServer::Impl
//  (RenderServerImpl.hpp, project-visible), casting the dispatcher's void* user_state.
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp>         // Dispatcher, Ctx, replyToCurrent, FeatureFactory
#include <lux/engine/render/comm/server/RenderServerImpl.hpp>     // GeneralRenderServer::Impl (assembly host) + handle_cast
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>   // typed-op register/unregister
#include <lux/engine/render/renderer/features/FeatureOpSend.hpp>  // send / sendWithReply / sendBulk
#include <lux/engine/render/comm/RenderProtocol.hpp>              // opcodes + instance payloads
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>
#include <lux/engine/render/renderer/features/meshstack/StandardMeshStackFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>        // MeshStackProxy: builder/RenderRequest/pushSharedBytes
#include <lux/engine/render/resources/mesh/InstanceResources.hpp> // InstanceSlot, sections, cull meta, properties
#include <lux/engine/render/resources/mesh/MeshResources.hpp>     // MeshResources, MeshCreateInfo, SSBOInitConfig
#include <lux/engine/render/resources/material/MaterialResources.hpp> // MaterialResources slotRecord (variant bucket / packed type)
#include <lux/engine/render/resources/vertex/MeshInputPool.hpp>   // bindless set-7 vertex-pool resolution
#include <lux/engine/description/Mesh.hpp>                        // rdesc::Mesh / rdesc::Vertex

#include <iostream>   // std::cerr (global-resource init failure)
#include <memory>
#include <vector>

#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx        = Dispatcher::Ctx;

    // Generic scene-resolution shim — stays in the core server (resolves a scene for
    // ANY feature, not just mesh), so we forward-declare and link to it:
    //   lookupScene : resolve a scene by id.
    // (The transform batch used to resolve via lookupCurrentBulkScene / SetActiveScene;
    //  G-04 made every TransformWriteEntry carry its own scene_id, so it now routes
    //  through lookupScene like every other op. SetActiveScene stays in the core server
    //  for any remaining legacy bulk path.)
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    // ── GLOBAL mesh arena (vertex 64MB + index 32MB) ──────────────────────────
    // External linkage: StandardMeshStackFeature::initAndAttachTo also triggers this
    // (it forward-declares it), so it is NOT in the anonymous namespace. Lazy +
    // idempotent — a server whose scenes never add StandardMeshStack and never upload a
    // mesh (pure 2D / headless / compute-only) never allocates the ~96MB.
    void ensureGlobalMeshResources(RenderContext& ctx)
    {
        auto& greg = ctx.globalRegistry();
        if (greg.find<MeshResources>())
            return;

        MeshResources::InitInfo info{
            .device                = &ctx.deviceContext(),
            .vertex_arena_bytes    = 64ull * 1024 * 1024,
            .index_arena_bytes     = 32ull * 1024 * 1024,
            .enable_device_address = false,
            .frames_in_flight      = ctx.framesInFlight(),
            .segments_ssbo_cfg     = SSBOInitConfig{
                .device_context         = &ctx.deviceContext(),
                .initial_dense_capacity = 16384,
                .slices                 = ctx.framesInFlight(),
                .allow_shader_write     = false,
                .clear_on_remove        = false,
            },
        };
        greg.emplace<MeshResources>();
        auto* mr = greg.find<MeshResources>();
        if (mr->init(info))
            mr->setDeferredQueue(&ctx.deferredDestroyQueue());
        else
            // On failure the emplaced object stays in the registry but UNINITIALIZED —
            // the registry has no erase, and re-emplacing would orphan a slot. We do not
            // retry (a failed global arena alloc is effectively fatal): consumers guard
            // every read via isInitialized() (serverUploadMesh/serverDestroyMesh), so the
            // dead instance can never be operated on.
            std::cerr << "[StandardMeshStack] global MeshResources init failed; meshes will not render.\n";
    }

    namespace
    {
        // ── Mesh server-side assembly (moved out of RenderServer.cpp in Stage C) ──
        // Internal to this TU: only the handlers below call them. They reach the
        // server's Vulkan stack through GeneralRenderServer::Impl (the void* user_state).

        // Concatenate a mesh's LOD index buffers ([LOD0 .. LODn]) into `storage` and fill
        // `counts` (per-LOD index counts, LOD0 first). A single-LOD mesh yields counts ==
        // { mesh.indices.size() }.
        void concatMeshLods(const lux::rdesc::Mesh& mesh,
                            std::vector<uint32_t>& storage,
                            std::vector<uint32_t>& counts)
        {
            std::size_t total = mesh.indices.size();
            for (const auto& l : mesh.lods) total += l.indices.size();
            storage.clear(); storage.reserve(total);
            counts.clear();  counts.reserve(mesh.lods.size() + 1);
            storage.insert(storage.end(), mesh.indices.begin(), mesh.indices.end());
            counts.push_back(static_cast<uint32_t>(mesh.indices.size()));
            for (const auto& l : mesh.lods)
            {
                storage.insert(storage.end(), l.indices.begin(), l.indices.end());
                counts.push_back(static_cast<uint32_t>(l.indices.size()));
            }
        }

        // Build one MeshSectionRecord per LOD level (LOD0 first) from the mesh's GPU
        // record. Returns the LOD count (>=1); `out` must hold kMaxMeshLod.
        uint32_t buildMeshSectionRecords(
            MeshResources* mesh_res,
            MeshHandle mesh_h,
            MeshSectionRecord out[kMaxMeshLod])
        {
            auto* gpu = mesh_res ? mesh_res->getGpuRecord(mesh_h) : nullptr;
            if (!gpu || !gpu->ready.load(std::memory_order_acquire))
            {
                out[0] = MeshSectionRecord{};   // not ready → single empty section
                return 1u;
            }

            const auto base_vertex =
                static_cast<int32_t>(gpu->vertex_buffer_range.offset / gpu->vertex_stride);
            const uint32_t n = gpu->lod_count == 0u ? 1u : gpu->lod_count;
            for (uint32_t i = 0; i < n; ++i)
            {
                out[i].first_index  = gpu->lod_index_first[i];
                out[i].index_count  = gpu->lod_index_count[i];
                out[i].base_vertex  = base_vertex;
                out[i].vertex_count = 0u;
            }
            return n;
        }

        void writeLocalBoundsFromMesh(
            InstanceResources* inst,
            MeshResources* mesh_res,
            InstanceSlot slot,
            MeshHandle mesh_h)
        {
            auto* cpu = mesh_res ? mesh_res->getCpuRecord(mesh_h) : nullptr;
            if (cpu && cpu->valid)
            {
                const auto center = cpu->local_bounds.center();
                const auto extent = cpu->local_bounds.extents();
                const float radius = extent.norm() * 0.5f;
                inst->setLocalBsphere(slot, center.x(), center.y(), center.z(), radius);
                return;
            }

            // No bounds — large radius so the instance always passes culling.
            inst->setLocalBsphere(slot, 0.f, 0.f, 0.f, 1e10f);
        }

        // Heavy mesh-instance assembly (was GeneralRenderServer::addMeshInstance):
        // global MeshResources/MaterialResources + per-LOD sections + variant bucket +
        // InstanceProperty (incl. bindless set-7 vertex pool) + per-LOD MDC cull meta.
        // Returns a null handle on any failure (the handler replies it).
        RenderObjectHandle serverAddMeshInstance(
            void* user_state, RenderSceneId scene_id,
            RMeshHandle mesh, RMaterialHandle material,
            const float* transform16, std::uint32_t flags,
            EGeometryKind geometry_kind, PassMask pass_mask, std::uint32_t user_meta_index)
        {
            auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);

            auto* scene = im.renderer_->getScene(scene_id);
            if (!scene)
                return RenderObjectHandle{};

            auto* inst     = scene->sceneRegistry().find<InstanceResources>();
            auto* mesh_res = im.render_ctx_->globalRegistry().find<MeshResources>();
            auto* mat_res  = im.render_ctx_->globalRegistry().find<MaterialResources>();
            if (!inst || !mesh_res || !mat_res)
                return RenderObjectHandle{};

            const RenderObjectHandle object = inst->allocateObject();
            const InstanceSlot slot = inst->resolveSlot(object);
            if (!object || !slot)
                return RenderObjectHandle{};

            // Mesh sections — one per LOD level (LOD0 first).
            auto mesh_h = handle_cast<MeshHandle>(mesh);
            MeshSectionRecord sections[kMaxMeshLod];
            const uint32_t lod_count = buildMeshSectionRecords(mesh_res, mesh_h, sections);
            uint32_t section_ids[kMaxMeshLod];
            for (uint32_t i = 0; i < lod_count; ++i)
            {
                section_ids[i] = inst->registerMeshSection(sections[i]);
                if (section_ids[i] == MeshSectionTable::kInvalidSectionId)
                {
                    for (uint32_t j = 0; j < i; ++j)
                        inst->unregisterMeshSection(section_ids[j]);
                    inst->freeObject(object);
                    return RenderObjectHandle{};
                }
            }

            // Material bucket
            auto mat_h  = handle_cast<MaterialHandle>(material);
            auto* mat_slot = mat_res->slotRecord(mat_h);
            uint32_t bucket_id = mat_slot ? mat_slot->variant_bucket : 0u;

            // Transform
            InstanceTransform xf{};
            std::memcpy(xf.world, transform16, sizeof(xf.world));
            inst->writeTransform(slot, xf);

            // Property
            InstanceProperty prop{};
            prop.object_id       = object.index;
            prop.layer_mask      = ~0u;
            prop.transform_index = slot.index;
            prop.flags           = flags;
            prop.material_index  = mat_slot ? mat_slot->local_slot.index : 0u;
            // OPAQUE: the material resource layer pre-packed this at submit time. The
            // core no longer names EShadingModel / calls packMaterialType.
            prop.material_type   = mat_slot ? mat_slot->packed_material_type : 0u;
            prop.pass_and_geometry =
                static_cast<uint32_t>(pass_mask)
              | (static_cast<uint32_t>(geometry_kind) << 16u);
            prop.user_meta_index = user_meta_index;

            // Static-mesh path resolves vertices through the bindless pool (set 7): look
            // up the per-scene MeshInputPool wrapping the global VBO, register lazily, and
            // write the pool slot into the property. For static meshes vertex_base ==
            // input_vertex_offset == mesh.base_vertex (skinned meshes override these via
            // applyBoneSkinningOne, pointing at the skinned-output transient pool).
            if (auto* mip = scene->sceneRegistry().find<MeshInputPool>())
            {
                const uint32_t pool_id = mip->ensureRegistered();
                if (pool_id != ~0u && mip->source())
                {
                    const auto h = mip->source()->handleForMesh(mesh_h);
                    if (h.valid())
                    {
                        prop.vertex_pool_id      = h.pool_id;
                        prop.vertex_base         = h.vertex_base;
                        prop.vertex_count        = h.vertex_count;
                        prop.input_vertex_offset = h.vertex_base;
                    }
                }
            }

            inst->writeProperty(slot, prop);

            // CullMeta — one MDC per LOD section; the cull shader picks the LOD per frame
            // and appends the instance to lod_mdc[lod].
            static_assert(kMaxMeshLod <= 4u,
                          "InstanceCullMeta.lod_mdc[] must hold kMaxMeshLod entries");
            InstanceCullMeta meta{};
            meta.bucket_id = bucket_id;
            meta.lod_count = lod_count;
            for (uint32_t i = 0; i < lod_count; ++i)
                meta.lod_mdc[i] = inst->mdcTable().registerInstance(
                    static_cast<uint8_t>(geometry_kind), bucket_id, section_ids[i]);
            inst->writeCullMeta(slot, meta);

            // Local bounds from mesh
            writeLocalBoundsFromMesh(inst, mesh_res, slot, mesh_h);

            return object;
        }

        // Heavy ASYNC mesh-data assembly (allocate + MeshTransferTask + worker submit).
        // Returns false on a SYNCHRONOUS allocate failure (the handler then sends the
        // failure reply); on success the reply is DEFERRED — the shared upload worker
        // emits MeshUploadedReply on completion (request-id correlated).
        bool serverUploadMesh(void* user_state, const lux::rdesc::Mesh& mesh,
                              VertexLayoutId layout, std::uint64_t request_id,
                              std::shared_ptr<const void> data_owner)
        {
            auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
            // The 96MB mesh arena is built lazily — uploading a mesh is one of the two
            // triggers (the other is StandardMeshStack attach). Idempotent.
            ensureGlobalMeshResources(*im.render_ctx_);
            auto* mesh_res = im.render_ctx_->globalRegistry().find<MeshResources>();
            // Guard on isInitialized(), not just non-null: ensureGlobalMeshResources leaves
            // the emplaced object in the registry even if init() failed (the registry has no
            // erase), so find<>() can hand back an unusable instance. Reject it here so we
            // never allocateOnly() on uninit state (would crash); the handler then sends a
            // failure reply (status!=0), preventing a permanently-hung client RenderRequest.
            if (!mesh_res || !mesh_res->isInitialized())
                return false;

            auto vdata = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(mesh.vertices.data()),
                mesh.vertices.size() * sizeof(lux::rdesc::Vertex));

            // Concatenate LOD index buffers into shared storage (pinned for the async
            // worker alongside the source mesh via the task's data_owner below).
            auto idx_storage = std::make_shared<std::vector<uint32_t>>();
            std::vector<uint32_t> lod_counts;
            concatMeshLods(mesh, *idx_storage, lod_counts);
            auto idata = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(idx_storage->data()),
                idx_storage->size() * sizeof(uint32_t));

            MeshCreateInfo ci{};
            ci.layout_id        = layout;
            ci.vertex_stride    = static_cast<uint32_t>(sizeof(lux::rdesc::Vertex));
            ci.vertex_buffer    = vdata;
            ci.index_buffer     = idata;
            ci.index_type       = EIndexType::UInt32;
            ci.lod_index_counts = lod_counts;
            if (mesh.bounds)
                ci.bounds = *mesh.bounds;

            auto result = mesh_res->allocateOnly(ci);
            if (!result)
                return false;  // synchronous failure — handler replies
            auto& alloc = result.value();

            MeshTransferTask task{};
            task.mesh_index   = alloc.handle.index;
            task.vbo_buf      = mesh_res->vertexBuffer(alloc.vbo_segment);
            task.vbo_offset   = alloc.vbo_range.offset;
            task.vbo_bytes    = alloc.vbo_range.size;
            task.vbo_data     = vdata.data();
            task.ibo_buf      = mesh_res->indexBuffer(alloc.ibo_segment);
            task.ibo_offset   = alloc.ibo_range.offset;
            task.ibo_bytes    = alloc.ibo_range.size;
            task.ibo_data     = idata.data();
            // Pin BOTH the source mesh (vdata) and the concatenated LOD index storage
            // (idata) for the async worker.
            task.data_owner   = std::make_shared<
                std::pair<std::shared_ptr<const void>, std::shared_ptr<std::vector<uint32_t>>>>(
                    std::move(data_owner), idx_storage);
            task.request_id   = request_id;
            task.resource_gen = alloc.handle.gen;
            im.upload_pool_->submitMeshTransfer(std::move(task));
            return true;
        }

        void serverDestroyMesh(void* user_state, RMeshHandle handle)
        {
            auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
            auto* mesh_res = im.render_ctx_->globalRegistry().find<MeshResources>();
            if (mesh_res && mesh_res->isInitialized())
                mesh_res->destroy(handle_cast<MeshHandle>(handle));
        }

        // Trivial null-guarded slot resolve (mirrors the retired core helper).
        InstanceSlot resolveInstanceSlot(InstanceResources* inst, RenderObjectHandle object)
        {
            if (!inst || !object)
                return InstanceSlot::invalid();
            return inst->resolveSlot(object);
        }

        // Resolve a scene's InstanceResources via PUBLIC API only. Null when the
        // scene has no StandardMeshStackFeature → the handler no-ops.
        InstanceResources* resolveInstances(Ctx& ctx, RenderSceneId scene_id, RenderScene*& sc_out)
        {
            sc_out = lookupScene(ctx.user_state, scene_id);
            return sc_out ? sc_out->sceneRegistry().find<InstanceResources>() : nullptr;
        }

        // ── Instance lifecycle ───────────────────────────────────────────────
        void handleAddMeshInstance(Ctx& ctx, const AddMeshInstancePayload& p)
        {
            const RenderObjectHandle object = serverAddMeshInstance(
                ctx.user_state, p.scene_id, p.mesh, p.material, p.transform,
                p.flags, p.geometry_kind, p.pass_mask, p.user_meta_index);
            replyToCurrent<AddMeshInstancePayload>(ctx, MeshInstanceSlotReply{object});
        }

        void handleRemoveMeshInstance(Ctx& ctx, const RemoveMeshInstancePayload& p)
        {
            RenderScene* sc = nullptr;
            auto* inst = resolveInstances(ctx, p.scene_id, sc);
            if (!inst) return;
            const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
            if (!inst->isAlive(slot)) return;

            // Unregister from every view that references this slot, then free
            // (freeObject unregisters all LOD MDCs + sections).
            sc->forEachActiveView([&](auto& view) {
                view.unregisterObject(p.object);
            });
            inst->freeObject(p.object);
        }

        void handleMakeInstanceVisibleForView(Ctx& ctx, const MakeInstanceVisibleForViewPayload& p)
        {
            RenderScene* sc = nullptr;
            auto* inst = resolveInstances(ctx, p.scene_id, sc);
            if (!inst) return;
            auto* view = sc->getView(p.view);
            if (!view) return;
            const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
            if (!inst->isAlive(slot)) return;

            // Derive bucket_id from the already-written cull meta.
            const auto& meta = inst->cullMetaAt(slot);
            view->registerObject(p.object, meta.bucket_id);
        }

        void handleHideInstanceFromView(Ctx& ctx, const HideInstanceFromViewPayload& p)
        {
            RenderScene* sc = nullptr;
            auto* inst = resolveInstances(ctx, p.scene_id, sc);
            if (!inst) return;
            const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
            if (!inst->isAlive(slot)) return;
            if (auto* view = sc->getView(p.view))
                view->unregisterObject(p.object);
        }

        void handleUpdateInstanceFlags(Ctx& ctx, const UpdateInstanceFlagsPayload& p)
        {
            RenderScene* sc = nullptr;
            auto* inst = resolveInstances(ctx, p.scene_id, sc);
            if (!inst) return;
            const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
            if (!inst->isAlive(slot)) return;
            auto& prop = inst->propertyAt(slot);
            prop.flags = p.flags;
            inst->markPropertyDirty(slot);
        }

        void handleUpdateInstanceRenderState(Ctx& ctx, const UpdateInstanceRenderStatePayload& p)
        {
            RenderScene* sc = nullptr;
            auto* inst = resolveInstances(ctx, p.scene_id, sc);
            if (!inst) return;
            const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
            if (!inst->isAlive(slot)) return;
            inst->setRenderState(slot, p.geometry_kind, p.pass_mask);
        }

        void handleUpdateInstanceUserMeta(Ctx& ctx, const UpdateInstanceUserMetaPayload& p)
        {
            RenderScene* sc = nullptr;
            auto* inst = resolveInstances(ctx, p.scene_id, sc);
            if (!inst) return;
            const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
            if (!inst->isAlive(slot)) return;
            auto& prop = inst->propertyAt(slot);
            prop.user_meta_index = p.user_meta_index;
            inst->markPropertyDirty(slot);
        }

        // ── Per-frame transform batch (each entry self-routes by scene_id, G-04) ──
        void handleTransformBatch(Ctx& ctx, std::span<const TransformWriteEntry> entries)
        {
            // No SetActiveScene dependency: resolve each entry's OWN scene, so batches
            // interleaved from different scenes (editor main + preview) never cross.
            // Batches are typically single-scene (the mesh bridge sends one entry per
            // instance), so the per-entry resolve is cheap.
            for (const auto& e : entries)
            {
                RenderScene* sc = nullptr;
                auto* inst = resolveInstances(ctx, e.scene_id, sc);
                if (!inst) continue;
                const InstanceSlot slot = resolveInstanceSlot(inst, e.object);
                if (!inst->isAlive(slot)) continue;
                InstanceTransform xf{};
                std::memcpy(xf.world, e.transform, sizeof(xf.world));
                inst->writeTransform(slot, xf);
            }
        }

        // ── Mesh DATA upload/destroy (global arena; no scene_id) ───────────────
        void handleUploadMesh(Ctx& ctx, const UploadMeshPayload& p)
        {
            // Resolve the shared-owned rdesc::Mesh (the owner travels into the async
            // worker to keep vdata/idata alive past this handler — see serverUploadMesh).
            auto view = resolveExternalDataView(ctx.program, p.mesh_desc);
            const auto* mesh = reinterpret_cast<const lux::rdesc::Mesh*>(view.bytes.data());
            // Success replies are DEFERRED by the shared upload worker (MeshUploadedReply,
            // request-id correlated). Only a synchronous allocate failure replies here.
            if (!serverUploadMesh(ctx.user_state, *mesh, p.layout_id,
                                  ctx.currentRequestId(), std::move(view.owner)))
                replyToCurrent<UploadMeshPayload>(ctx, MeshUploadedReply{RMeshHandle{}, 1u});
        }

        void handleDestroyMesh(Ctx& ctx, const DestroyMeshPayload& p)
        {
            serverDestroyMesh(ctx.user_state, p.handle);
        }
    } // namespace

    // ── Uniform factory interface ────────────────────────────────────────────
    static FeatureHandle standardMeshStackCreateFn(void* scene_ptr, const void* /*param*/, size_t /*sz*/)
    {
        auto* sc = static_cast<RenderScene*>(scene_ptr);
        StandardMeshStackFeature::Config cfg{};   // no per-scene comm config
        return sc->addFeature<StandardMeshStackFeature>(cfg);
    }

    // Typed-op: register/unregister generated from the op list (kind picks
    // Unary/Bulk + opcode). Handlers above are the only hand-written part. The
    // declared order == FeatureOpIds order on the client.
    using MeshStackOps = FeatureOpRegistrar<
        ServerOp<AddMeshInstanceOp,             &handleAddMeshInstance>,
        ServerOp<RemoveMeshInstanceOp,          &handleRemoveMeshInstance>,
        ServerOp<MakeInstanceVisibleForViewOp,  &handleMakeInstanceVisibleForView>,
        ServerOp<HideInstanceFromViewOp,        &handleHideInstanceFromView>,
        ServerOp<UpdateInstanceFlagsOp,         &handleUpdateInstanceFlags>,
        ServerOp<UpdateInstanceRenderStateOp,   &handleUpdateInstanceRenderState>,
        ServerOp<UpdateInstanceUserMetaOp,      &handleUpdateInstanceUserMeta>,
        ServerOp<TransformBatchOp,              &handleTransformBatch>,
        ServerOp<UploadMeshOp,                  &handleUploadMesh>,
        ServerOp<DestroyMeshOp,                 &handleDestroyMesh>>;

    const FeatureFactory kStandardMeshStackFeatureFactory{
        &standardMeshStackCreateFn,
        &MeshStackOps::registerAll,
        &MeshStackOps::unregisterAll,
        "StandardMeshStack",
    };

    // ── Client-side proxy (typed-op send-routing) ─────────────────────────────
    RenderRequest<MeshInstanceSlotReply> MeshStackProxy::addMeshInstance(
        RenderSceneId scene_id, RMeshHandle mesh, RMaterialHandle material,
        const float transform[16], std::uint32_t flags,
        EGeometryKind geometry_kind, PassMask pass_mask, std::uint32_t user_meta_index)
    {
        AddMeshInstancePayload p{};
        p.scene_id        = scene_id;
        p.mesh            = mesh;
        p.material        = material;
        std::memcpy(p.transform, transform, sizeof(p.transform));
        p.flags           = flags;
        p.geometry_kind   = geometry_kind;
        p.pass_mask       = pass_mask;
        p.user_meta_index = user_meta_index;
        return sendWithReply<AddMeshInstanceOp>(*session_, ops_, p);
    }

    void MeshStackProxy::removeMeshInstance(RenderSceneId scene_id, RenderObjectHandle object)
    {
        RemoveMeshInstancePayload p{};
        p.scene_id = scene_id;
        p.object   = object;
        send<RemoveMeshInstanceOp>(*session_, ops_, p);
    }

    void MeshStackProxy::makeInstanceVisibleForView(RenderSceneId scene_id, ViewHandle view, RenderObjectHandle object)
    {
        MakeInstanceVisibleForViewPayload p{};
        p.scene_id = scene_id;
        p.view     = view;
        p.object   = object;
        send<MakeInstanceVisibleForViewOp>(*session_, ops_, p);
    }

    void MeshStackProxy::hideInstanceFromView(RenderSceneId scene_id, ViewHandle view, RenderObjectHandle object)
    {
        HideInstanceFromViewPayload p{};
        p.scene_id = scene_id;
        p.view     = view;
        p.object   = object;
        send<HideInstanceFromViewOp>(*session_, ops_, p);
    }

    void MeshStackProxy::updateInstanceFlags(RenderSceneId scene_id, RenderObjectHandle object, std::uint32_t flags)
    {
        UpdateInstanceFlagsPayload p{};
        p.scene_id = scene_id;
        p.object   = object;
        p.flags    = flags;
        send<UpdateInstanceFlagsOp>(*session_, ops_, p);
    }

    void MeshStackProxy::updateInstanceRenderState(RenderSceneId scene_id, RenderObjectHandle object,
                                                   EGeometryKind geometry_kind, PassMask pass_mask)
    {
        UpdateInstanceRenderStatePayload p{};
        p.scene_id      = scene_id;
        p.object        = object;
        p.geometry_kind = geometry_kind;
        p.pass_mask     = pass_mask;
        send<UpdateInstanceRenderStateOp>(*session_, ops_, p);
    }

    void MeshStackProxy::updateInstanceUserMeta(RenderSceneId scene_id, RenderObjectHandle object, std::uint32_t user_meta_index)
    {
        UpdateInstanceUserMetaPayload p{};
        p.scene_id        = scene_id;
        p.object          = object;
        p.user_meta_index = user_meta_index;
        send<UpdateInstanceUserMetaOp>(*session_, ops_, p);
    }

    void MeshStackProxy::updateTransform(RenderSceneId scene_id, RenderObjectHandle object, const float transform[16])
    {
        TransformWriteEntry e{};
        e.scene_id = scene_id;
        e.object   = object;
        std::memcpy(e.transform, transform, sizeof(e.transform));
        sendBulk<TransformBatchOp>(*session_, ops_, std::span<const TransformWriteEntry>{&e, 1});
    }

    void MeshStackProxy::updateTransforms(std::span<const TransformWriteEntry> entries)
    {
        sendBulk<TransformBatchOp>(*session_, ops_, entries);
    }

    RenderRequest<MeshUploadedReply> MeshStackProxy::uploadMesh(
        const lux::rdesc::Mesh& mesh, VertexLayoutId layout_id)
    {
        // Deep-COPY the Mesh into a shared buffer the async upload worker keeps alive
        // (the GPU transfer outlives submitFrame) — NOT a borrow. Same byte stream as
        // the retired RenderSession::uploadMesh (pushSharedBytes + pushResource).
        auto owned = std::make_shared<lux::rdesc::Mesh>(mesh);
        UploadMeshPayload p{};
        p.layout_id = layout_id;
        p.mesh_desc = session_->builder().pushSharedBytes(
            std::static_pointer_cast<const void>(owned),
            reinterpret_cast<const std::byte*>(owned.get()),
            static_cast<std::uint32_t>(sizeof(lux::rdesc::Mesh)));
        return sendWithReply<UploadMeshOp>(*session_, ops_, p);
    }

    void MeshStackProxy::destroyMesh(RMeshHandle handle)
    {
        DestroyMeshPayload p{};
        p.handle = handle;
        send<DestroyMeshOp>(*session_, ops_, p);
    }

} // namespace lux::render
