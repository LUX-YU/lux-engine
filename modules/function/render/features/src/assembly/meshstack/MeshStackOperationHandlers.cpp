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
//  and async mesh-data upload) lives HERE — internal to this TU; the core RenderServer
//  no longer hosts any mesh body. 它经**窄 shim** 取服务端的东西
//  (lookupScene / lookupRenderContext / lookupUploadPool,定义在 RenderServer.cpp),
//  **不再 include 服务端的私有 Impl 头** —— 见那组 shim 处关于"24 个装配 TU 里
//  22 个遵守这条约定"的说明。
//  (全局网格竞技场的构造器已下沉 L3,见 resources/mesh/MeshResources.cpp。)
// ============================================================================

#include <lux/engine/render/comm/server/RenderServer.hpp> // Dispatcher, Ctx, replyToCurrent, FeatureFactory
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp> // handle_cast
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp> // lookupRenderContext 的返回类型
#include <lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp>
#include <lux/engine/render/comm/server/FeatureOpRegistrar.hpp>          // typed-op register/unregister
#include <lux/engine/function/render/client/protocol/FeatureFactory.hpp> // FeatureFactory / GenericOkReply
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/render/renderer/features/meshstack/StandardMeshStackFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp> // InstanceSlot, sections, cull meta, properties
#include <lux/engine/render/resources/mesh/MeshResources.hpp>     // MeshResources, MeshCreateInfo, SSBOInitConfig
#include <lux/engine/render/resources/material/MaterialResources.hpp>
// MaterialResources slotRecord (variant bucket / packed type)
#include <lux/engine/render/resources/vertex/StaticVertexPoolSet.hpp>
#include <lux/engine/render/renderer/features/meshstack/MeshInstanceAssembly.hpp>
#include <lux/engine/description/Mesh.hpp> // rdesc::Mesh / rdesc::Vertex

#include <algorithm>
#include <memory>
#include <limits>
#include <vector>

#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

namespace lux::render
{
    using Dispatcher = GeneralRenderServer::Dispatcher;
    using Ctx = Dispatcher::Ctx;

    // Generic scene-resolution shim — stays in the core server (resolves a scene for
    // ANY feature, not just mesh), so we forward-declare and link to it:
    //   lookupScene : resolve a scene by id.
    // (The transform batch used to resolve via lookupCurrentBulkScene / SetActiveScene;
    //  G-04 made every TransformWriteEntry carry its own scene_id, so it now routes
    //  through lookupScene like every other op. SetActiveScene stays in the core server
    //  for any remaining legacy bulk path.)
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);
    RenderContext* lookupRenderContext(void* user_state);
    GpuTransferPipeline* lookupTransferPipeline(void* user_state);

    //(ensureGlobalMeshResources 已下沉到 L3 的
    // src/render/resources/mesh/MeshResources.cpp —— 它的函数体里没有一个协议
    // 词汇,住在装配层的唯一原因是历史。留在这里会让 L4 的
    // StandardMeshStackFeature 必须前向声明它,形成层规则看不见的
    // L4→L6 链接期依赖。声明见 resources/mesh/MeshResources.hpp。)

    namespace
    {
        // ── Mesh server-side assembly (moved out of RenderServer.cpp in Stage C) ──
        // Internal to this TU: only the handlers below call them. They reach the
        // 服务端的东西经窄 shim 取(lookupScene / lookupRenderContext / lookupUploadPool)。

        // Concatenate a mesh's LOD index buffers ([LOD0 .. LODn]) into `storage` and fill
        // `counts` (per-LOD index counts, LOD0 first). A single-LOD mesh yields counts ==
        // { mesh.indices.size() }.
        void concatMeshLods(const lux::rdesc::Mesh& mesh, std::vector<uint32_t>& storage, std::vector<uint32_t>& counts)
        {
            std::size_t total = mesh.indices.size();
            for (const auto& l : mesh.lods)
                total += l.indices.size();
            storage.clear();
            storage.reserve(total);
            counts.clear();
            counts.reserve(mesh.lods.size() + 1);
            storage.insert(storage.end(), mesh.indices.begin(), mesh.indices.end());
            counts.push_back(static_cast<uint32_t>(mesh.indices.size()));
            for (const auto& l : mesh.lods)
            {
                storage.insert(storage.end(), l.indices.begin(), l.indices.end());
                counts.push_back(static_cast<uint32_t>(l.indices.size()));
            }
        }

        struct MeshIndexUploadStorage final
        {
            std::vector<std::uint32_t> wide;
            std::vector<std::uint16_t> compact;

            [[nodiscard]] std::span<const std::byte> bytes() const noexcept
            {
                if (!compact.empty())
                {
                    return {reinterpret_cast<const std::byte*>(compact.data()), compact.size() * sizeof(std::uint16_t)};
                }
                return {reinterpret_cast<const std::byte*>(wide.data()), wide.size() * sizeof(std::uint32_t)};
            }
        };

        // Build one MeshSectionRecord per LOD level (LOD0 first) from the mesh's GPU
        // record. Returns the LOD count (>=1); `out` must hold kMaxMeshLod.
        uint32_t buildMeshSectionRecords(MeshResources* mesh_res, MeshHandle mesh_h, MeshSectionRecord out[kMaxMeshLod])
        {
            auto* gpu = mesh_res ? mesh_res->getGpuRecord(mesh_h) : nullptr;
            if (!gpu || !gpu->ready)
            {
                out[0] = MeshSectionRecord{}; // not ready → single empty section
                return 1u;
            }

            const auto base_vertex = static_cast<int32_t>(gpu->vertex_buffer_range.offset / gpu->vertex_stride);
            const uint32_t n = gpu->lod_count == 0u ? 1u : gpu->lod_count;
            for (uint32_t i = 0; i < n; ++i)
            {
                out[i].first_index = gpu->lod_index_first[i];
                out[i].index_count = gpu->lod_index_count[i];
                out[i].base_vertex = base_vertex;
                out[i].vertex_count = 0u;
            }
            return n;
        }

        void
        writeLocalBoundsFromMesh(InstanceResources* inst, MeshResources* mesh_res, InstanceSlot slot, MeshHandle mesh_h)
        {
            auto* cpu = mesh_res ? mesh_res->getCpuRecord(mesh_h) : nullptr;
            const bool has_valid_bounds = cpu != nullptr && cpu->valid && cpu->local_bounds.isValid() &&
                cpu->local_bounds.min.allFinite() && cpu->local_bounds.max.allFinite();
            if (has_valid_bounds)
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
            void* user_state,
            RenderSceneId scene_id,
            RMeshHandle mesh,
            RMaterialHandle material,
            const RenderSpatialTransform3D& transform,
            std::uint32_t flags,
            EGeometryKind geometry_kind,
            PassMask pass_mask,
            std::uint32_t user_meta_index,
            std::uint32_t transition_milliseconds,
            std::uint32_t transition_seed,
            MeshInstanceCreateStatus& out_status
        ) // distinguishes config vs capacity failure
        {
            auto* rctx = lookupRenderContext(user_state);
            auto* scene = lookupScene(user_state, scene_id);
            if (!scene || !rctx)
            {
                out_status = MeshInstanceCreateStatus::InvalidConfiguration;
                return RenderObjectHandle{};
            } // dead/wrong scene_id

            auto* inst = scene->sceneRegistry().find<InstanceResources>();
            auto* mesh_res = rctx->globalRegistry().find<MeshResources>();
            auto* mat_res = rctx->globalRegistry().find<MaterialResources>();
            if (!inst || !mesh_res || !mat_res)
            {
                out_status = MeshInstanceCreateStatus::InvalidConfiguration;
                return RenderObjectHandle{};
            } // mesh-stack feature absent

            const auto mesh_h = handle_cast<MeshHandle>(mesh);
            const auto mat_h = handle_cast<MaterialHandle>(material);

            const auto* gpu_rec = mesh_res->getGpuRecord(mesh_h);
            if (gpu_rec == nullptr)
            {
                out_status = MeshInstanceCreateStatus::InvalidConfiguration;
                return {};
            }

            // Instances, including Render Ghosts, own explicit render-thread
            // references. Asset destroy requests may arrive as soon as the ECS
            // owner disappears; these pins keep both resources alive until the
            // instance slot itself is retired.
            if (!mesh_res->retainForInstance(mesh_h))
            {
                out_status = MeshInstanceCreateStatus::InvalidConfiguration;
                return {};
            }
            if (!mat_res->retainForInstance(mat_h))
            {
                mesh_res->releaseFromInstance(mesh_h);
                out_status = MeshInstanceCreateStatus::InvalidConfiguration;
                return {};
            }
            const auto release_resources = [&]() noexcept {
                mat_res->releaseFromInstance(mat_h);
                mesh_res->releaseFromInstance(mesh_h);
            };

            const RenderObjectHandle object = inst->allocateObject();
            const InstanceSlot slot = inst->resolveSlot(object);
            if (!object || !slot)
            {
                release_resources();
                out_status = MeshInstanceCreateStatus::CapacityExhausted;
                return RenderObjectHandle{};
            } // instance pool exhausted

            // Mesh sections — one per LOD level (LOD0 first). Segment and index
            // type participate in CPU dedup/MDC identity because first_index is
            // relative to the VkBuffer bound for that lane.
            MeshSectionRecord sections[kMaxMeshLod];
            const uint32_t lod_count = buildMeshSectionRecords(mesh_res, mesh_h, sections);
            uint32_t section_ids[kMaxMeshLod];
            for (uint32_t i = 0; i < lod_count; ++i)
            {
                section_ids[i] = inst->registerMeshSection(
                    sections[i],
                    gpu_rec->ibo_segment,
                    gpu_rec->index_type == EIndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32
                );
                if (section_ids[i] == MeshSectionTable::kInvalidSectionId)
                {
                    for (uint32_t j = 0; j < i; ++j)
                        inst->unregisterMeshSection(section_ids[j]);
                    inst->freeObject(object);
                    release_resources();
                    out_status = MeshInstanceCreateStatus::CapacityExhausted;
                    return RenderObjectHandle{}; // section table exhausted
                }
            }

            // Material bucket
            auto* mat_slot = mat_res->slotRecord(mat_h);
            uint32_t bucket_id = mat_slot ? mat_slot->variant_bucket : 0u;

            // Transform
            InstanceTransform xf{};
            xf = transform;
            inst->writeTransform(slot, xf);

            // Property
            InstanceProperty prop{};
            prop.object_id = object.index;
            prop.layer_mask = ~0u;
            prop.transform_index = slot.index;
            prop.flags = flags;
            prop.material_index = mat_slot ? mat_slot->local_slot.index : 0u;
            // OPAQUE: the material resource layer pre-packed this at submit time. The
            // core no longer names EShadingModel / calls packMaterialType.
            prop.material_type = mat_slot ? mat_slot->packed_material_type : 0u;
            prop.pass_and_geometry = static_cast<uint32_t>(pass_mask) | (static_cast<uint32_t>(geometry_kind) << 16u);
            prop.user_meta_index = user_meta_index;
            if (transition_milliseconds != 0u && transition_seed != 0u && !hasPass(pass_mask, eTransparent))
            {
                prop.transition_start_time = scene->sceneTime();
                prop.transition_duration = static_cast<float>(transition_milliseconds) / 1000.0f;
                prop.transition_seed = transition_seed;
                prop.transition_flags = 1u;
            }

            // Static-mesh path resolves vertices through the bindless pool (set 7): look
            // up the per-scene StaticVertexPoolSet entry for this VBO segment,
            // register lazily, and
            // write the pool slot into the property. For static meshes vertex_base ==
            // input_vertex_offset == mesh.base_vertex (skinned meshes override these via
            // applyBoneSkinningOne, pointing at the skinned-output transient pool).
            if (auto* pools = scene->sceneRegistry().find<StaticVertexPoolSet>())
            {
                const auto vertex = pools->handleForMesh(mesh_h);
                if (vertex.valid())
                {
                    prop.vertex_pool_id = vertex.pool_id;
                    prop.vertex_base = vertex.vertex_base;
                    prop.vertex_count = vertex.vertex_count;
                    prop.input_vertex_offset = vertex.vertex_base;
                }
            }

            inst->writeProperty(slot, prop);

            // CullMeta — one MDC per LOD section; the cull shader picks the LOD per frame
            // and appends the instance to lod_mdc[lod].
            static_assert(kMaxMeshLod <= 4u, "InstanceCullMeta.lod_mdc[] must hold kMaxMeshLod entries");
            InstanceCullMeta meta{};
            meta.bucket_id = bucket_id;
            meta.lod_count = lod_count;
            for (uint32_t i = 0; i < lod_count; ++i)
                meta.lod_mdc[i] = inst->mdcTable().registerInstance(
                    static_cast<uint8_t>(geometry_kind),
                    bucket_id,
                    section_ids[i],
                    gpu_rec->ibo_segment,
                    gpu_rec->index_type == EIndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32
                );
            inst->writeCullMeta(slot, meta);

            // Local bounds from mesh
            writeLocalBoundsFromMesh(inst, mesh_res, slot, mesh_h);

            if (!inst->bindResources(object, InstanceResources::ResourceBinding{mesh_h, mat_h}))
            {
                inst->freeObject(object);
                release_resources();
                out_status = MeshInstanceCreateStatus::InvalidConfiguration;
                return {};
            }

            out_status = MeshInstanceCreateStatus::Ok;
            return object;
        }

        // Heavy ASYNC mesh-data assembly (allocate + MeshTransferTask + worker submit).
        // Returns false on a SYNCHRONOUS allocate failure (the handler then sends the
        // failure reply); on success the reply is DEFERRED — the shared upload worker
        // emits MeshUploadedReply on completion (request-id correlated).
        bool serverUploadMesh(
            void* user_state,
            const lux::rdesc::Mesh& mesh,
            VertexLayoutId layout,
            std::uint64_t request_id,
            std::shared_ptr<const void> data_owner,
            lux::render::CapacityShortfallWire* shortfall_output
        )
        {
            auto* rctx = lookupRenderContext(user_state);
            if (!rctx)
                return false;
            // The 96MB mesh arena is built lazily — uploading a mesh is one of the two
            // triggers (the other is StandardMeshStack attach). Idempotent.
            if (!ensureGlobalMeshResources(*rctx))
                return false;
            // ensureGlobalMeshResources 成功 ⇒ 注册表里有一个**已初始化**的实例:
            // 它走 ensure<T>(init_args),只在 init 成功后才发布。此前它是
            // emplace → init,失败时把未初始化的对象留在(无 erase 的)注册表里,
            // 于是这里必须复查 isInitialized(),否则 allocateOnly() 会崩。
            auto& mesh_res = rctx->globalRegistry().must<MeshResources>();

            auto vdata = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(mesh.vertices.data()),
                mesh.vertices.size() * sizeof(lux::rdesc::Vertex)
            );

            // Concatenate LOD index buffers into shared storage (pinned for the async
            // worker alongside the source mesh via the task's data_owner below).
            auto idx_storage = std::make_shared<MeshIndexUploadStorage>();
            std::vector<uint32_t> lod_counts;
            concatMeshLods(mesh, idx_storage->wide, lod_counts);
            const bool use_compact_indices = std::ranges::all_of(idx_storage->wide, [](std::uint32_t index) {
                return index <= std::numeric_limits<std::uint16_t>::max();
            }
            );
            if (use_compact_indices)
            {
                idx_storage->compact.reserve(idx_storage->wide.size());
                for (const auto index : idx_storage->wide)
                {
                    idx_storage->compact.push_back(static_cast<std::uint16_t>(index));
                }
                idx_storage->wide.clear();
                idx_storage->wide.shrink_to_fit();
            }
            const auto idata = idx_storage->bytes();

            MeshCreateInfo ci{};
            ci.layout_id = layout;
            ci.vertex_stride = static_cast<uint32_t>(sizeof(lux::rdesc::Vertex));
            ci.vertex_buffer = vdata;
            ci.index_buffer = idata;
            ci.index_type = use_compact_indices ? EIndexType::UInt16 : EIndexType::UInt32;
            ci.lod_index_counts = lod_counts;
            if (mesh.bounds)
                ci.bounds = *mesh.bounds;
            else if (!mesh.vertices.empty())
            {
                lux::math::AABB computed;
                for (const auto& vertex : mesh.vertices)
                    computed.merge(vertex.position);
                if (computed.isValid() && computed.min.allFinite() && computed.max.allFinite())
                {
                    ci.bounds = computed;
                }
            }

            auto result = mesh_res.allocateOnly(ci);
            if (!result)
            {
                if (shortfall_output && mesh_res.lastCapacityShortfall())
                {
                    *shortfall_output = lux::render::capacityShortfallWire(*mesh_res.lastCapacityShortfall());
                }
                return false; // synchronous failure — handler replies
            }
            auto& alloc = result.value();

            MeshTransferTask task{};
            task.mesh_index = alloc.handle.index;
            task.vbo_buf = mesh_res.vertexBuffer(alloc.vbo_segment);
            task.vbo_offset = alloc.vbo_range.offset;
            task.vbo_bytes = alloc.vbo_range.size;
            task.vbo_data = vdata.data();
            task.ibo_buf = mesh_res.indexBuffer(alloc.ibo_segment);
            task.ibo_offset = alloc.ibo_range.offset;
            task.ibo_bytes = alloc.ibo_range.size;
            task.ibo_data = idata.data();
            // Pin BOTH the source mesh (vdata) and the concatenated LOD index storage
            // (idata) for the async worker.
            task.data_owner =
                std::make_shared<std::pair<std::shared_ptr<const void>, std::shared_ptr<MeshIndexUploadStorage>>>(
                    std::move(data_owner),
                    idx_storage
                );
            task.request_id = request_id;
            task.resource_gen = alloc.handle.gen;
            if (!lookupTransferPipeline(user_state)->submitMeshTransfer(std::move(task)))
            {
                mesh_res.destroy(alloc.handle);
                return false;
            }
            return true;
        }

        void serverDestroyMesh(void* user_state, RMeshHandle handle)
        {
            auto* rctx = lookupRenderContext(user_state);
            if (!rctx)
                return;
            auto* mesh_res = rctx->globalRegistry().find<MeshResources>();
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

    } // anonymous namespace (helpers)

    RenderObjectHandle detail::createMeshInstance(
        void* server_state,
        RenderSceneId scene_id,
        RMeshHandle mesh,
        RMaterialHandle material,
        const RenderSpatialTransform3D& transform,
        std::uint32_t flags,
        EGeometryKind geometry_kind,
        PassMask pass_mask,
        std::uint32_t user_meta_index,
        bool visible_in_all_views,
        MeshInstanceCreateStatus& status,
        std::uint32_t transition_milliseconds,
        std::uint32_t transition_seed
    )
    {
        const auto object = serverAddMeshInstance(
            server_state,
            scene_id,
            mesh,
            material,
            transform,
            flags,
            geometry_kind,
            pass_mask,
            user_meta_index,
            transition_milliseconds,
            transition_seed,
            status
        );
        if (!object || !visible_in_all_views)
            return object;

        auto* scene = lookupScene(server_state, scene_id);
        auto* instances = scene ? scene->sceneRegistry().find<InstanceResources>() : nullptr;
        const auto slot = resolveInstanceSlot(instances, object);
        if (!scene || !instances || !instances->isAlive(slot))
        {
            status = MeshInstanceCreateStatus::InvalidConfiguration;
            if (scene && instances && instances->isAlive(slot))
                detail::destroyMeshInstance(*scene, *lookupRenderContext(server_state), object);
            return {};
        }
        const auto bucket = instances->cullMetaAt(slot).bucket_id;
        scene->forEachActiveView([object, bucket](auto& view) { view.registerObject(object, bucket); });
        return object;
    }

    bool detail::reconfigureMeshInstances(
        void* server_state,
        RenderSceneId scene_id,
        std::span<const MeshInstanceRevision> revisions,
        MeshInstanceCreateStatus& status
    )
    {
        struct PreparedRevision final
        {
            MeshInstanceRevision input;
            InstanceResources::ResourceBinding previous_binding;
            InstanceResources::ResourceBinding next_binding;
            InstanceProperty property;
            InstanceCullMeta cull_meta;
            std::uint32_t section_ids[kMaxMeshLod]{};
            bool reuses_layout{false};
        };

        auto* context = lookupRenderContext(server_state);
        auto* scene = lookupScene(server_state, scene_id);
        auto* instances = scene ? scene->sceneRegistry().find<InstanceResources>() : nullptr;
        auto* meshes = context ? context->globalRegistry().find<MeshResources>() : nullptr;
        auto* materials = context ? context->globalRegistry().find<MaterialResources>() : nullptr;
        const bool is_missing_context = context == nullptr;
        const bool is_missing_scene = scene == nullptr;
        const bool is_missing_instances = instances == nullptr;
        const bool is_missing_meshes = meshes == nullptr;
        const bool is_missing_materials = materials == nullptr;
        const bool is_invalid_configuration = is_missing_context || is_missing_scene || is_missing_instances ||
            is_missing_meshes || is_missing_materials;
        if (is_invalid_configuration)
        {
            status = MeshInstanceCreateStatus::InvalidConfiguration;
            return false;
        }

        std::vector<PreparedRevision> prepared;
        prepared.reserve(revisions.size());
        const auto rollback = [&]() noexcept {
            for (auto& revision : prepared)
            {
                if (revision.reuses_layout)
                    continue;
                for (std::uint32_t lod = 0u; lod < revision.cull_meta.lod_count; ++lod)
                {
                    instances->mdcTable().unregisterInstance(revision.cull_meta.lod_mdc[lod]);
                    instances->unregisterMeshSection(revision.section_ids[lod]);
                }
                materials->releaseFromInstance(revision.next_binding.material);
                meshes->releaseFromInstance(revision.next_binding.mesh);
            }
            prepared.clear();
        };

        for (const auto& input : revisions)
        {
            const auto slot = instances->resolveSlot(input.object);
            const auto previous = instances->resourceBinding(input.object);
            const auto mesh = handle_cast<MeshHandle>(input.mesh);
            const auto material = handle_cast<MaterialHandle>(input.material);
            const auto* gpu_record = meshes->getGpuRecord(mesh);
            auto* material_slot = materials->slotRecord(material);
            const bool is_dead_instance = !instances->isAlive(slot);
            const bool is_missing_previous = previous == nullptr;
            const bool is_missing_gpu_record = gpu_record == nullptr;
            const bool is_missing_material_slot = material_slot == nullptr;
            const bool is_invalid_input = is_dead_instance || is_missing_previous || is_missing_gpu_record ||
                is_missing_material_slot;
            if (is_invalid_input)
            {
                rollback();
                status = MeshInstanceCreateStatus::InvalidConfiguration;
                return false;
            }

            const auto pass_and_geometry =
                static_cast<std::uint32_t>(input.pass_mask) | (static_cast<std::uint32_t>(input.geometry_kind) << 16u);
            if (previous->mesh == mesh && previous->material == material &&
                instances->propertyAt(slot).pass_and_geometry == pass_and_geometry)
            {
                PreparedRevision next{};
                next.input = input;
                next.previous_binding = *previous;
                next.next_binding = *previous;
                next.property = instances->propertyAt(slot);
                next.property.flags = input.flags;
                next.property.user_meta_index = input.user_meta_index;
                next.property.rgba8 = input.rgba8;
                next.reuses_layout = true;
                prepared.push_back(std::move(next));
                continue;
            }

            if (!meshes->retainForInstance(mesh))
            {
                rollback();
                status = MeshInstanceCreateStatus::InvalidConfiguration;
                return false;
            }
            if (!materials->retainForInstance(material))
            {
                meshes->releaseFromInstance(mesh);
                rollback();
                status = MeshInstanceCreateStatus::InvalidConfiguration;
                return false;
            }

            PreparedRevision next{};
            next.input = input;
            next.previous_binding = *previous;
            next.next_binding = {mesh, material};

            MeshSectionRecord sections[kMaxMeshLod]{};
            const auto lod_count = buildMeshSectionRecords(meshes, mesh, sections);
            bool section_failed = false;
            std::uint32_t registered_sections = 0u;
            for (; registered_sections < lod_count; ++registered_sections)
            {
                next.section_ids[registered_sections] = instances->registerMeshSection(
                    sections[registered_sections],
                    gpu_record->ibo_segment,
                    gpu_record->index_type == EIndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32
                );
                if (next.section_ids[registered_sections] == MeshSectionTable::kInvalidSectionId)
                {
                    section_failed = true;
                    break;
                }
            }
            if (section_failed)
            {
                for (std::uint32_t index = 0u; index < registered_sections; ++index)
                {
                    instances->unregisterMeshSection(next.section_ids[index]);
                }
                materials->releaseFromInstance(material);
                meshes->releaseFromInstance(mesh);
                rollback();
                status = MeshInstanceCreateStatus::CapacityExhausted;
                return false;
            }

            next.cull_meta.bucket_id = material_slot->variant_bucket;
            next.cull_meta.lod_count = lod_count;
            for (std::uint32_t lod = 0u; lod < lod_count; ++lod)
            {
                next.cull_meta.lod_mdc[lod] = instances->mdcTable().registerInstance(
                    static_cast<std::uint8_t>(input.geometry_kind),
                    material_slot->variant_bucket,
                    next.section_ids[lod],
                    gpu_record->ibo_segment,
                    gpu_record->index_type == EIndexType::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32
                );
            }

            next.property.object_id = input.object.index;
            next.property.layer_mask = ~0u;
            next.property.transform_index = slot.index;
            next.property.flags = input.flags;
            next.property.material_index = material_slot->local_slot.index;
            next.property.material_type = material_slot->packed_material_type;
            next.property.pass_and_geometry = pass_and_geometry;
            next.property.user_meta_index = input.user_meta_index;
            next.property.rgba8 = input.rgba8;
            if (auto* pools = scene->sceneRegistry().find<StaticVertexPoolSet>())
            {
                const auto vertex = pools->handleForMesh(mesh);
                if (vertex.valid())
                {
                    next.property.vertex_pool_id = vertex.pool_id;
                    next.property.vertex_base = vertex.vertex_base;
                    next.property.vertex_count = vertex.vertex_count;
                    next.property.input_vertex_offset = vertex.vertex_base;
                }
            }
            prepared.push_back(std::move(next));
        }

        for (const auto& revision : prepared)
        {
            scene->forEachActiveView([object = revision.input.object](auto& view) { view.unregisterObject(object); });
        }
        for (auto& revision : prepared)
        {
            const auto slot = instances->resolveSlot(revision.input.object);
            if (revision.reuses_layout)
            {
                instances->writeTransform(slot, revision.input.transform);
                instances->writeProperty(slot, revision.property);
                continue;
            }
            const auto previous = instances->replaceResources(revision.input.object, revision.next_binding);
            if (!previous)
            {
                renderFatal("Mesh instance resource binding vanished during "
                            "revision commit"
                );
            }
            instances->writeTransform(slot, revision.input.transform);
            instances->writeProperty(slot, revision.property);
            instances->writeCullMeta(slot, revision.cull_meta);
            writeLocalBoundsFromMesh(instances, meshes, slot, revision.next_binding.mesh);
            materials->releaseFromInstance(previous->material);
            meshes->releaseFromInstance(previous->mesh);
        }
        status = MeshInstanceCreateStatus::Ok;
        return true;
    }

    void detail::destroyMeshInstance(void* server_state, RenderSceneId scene_id, RenderObjectHandle object) noexcept
    {
        auto* scene = lookupScene(server_state, scene_id);
        auto* context = lookupRenderContext(server_state);
        if (!scene || !context)
            return;
        detail::destroyMeshInstance(*scene, *context, object);
    }

    void detail::destroyMeshInstance(RenderScene& scene, RenderContext& context, RenderObjectHandle object) noexcept
    {
        auto* instances = scene.sceneRegistry().find<InstanceResources>();
        const auto slot = resolveInstanceSlot(instances, object);
        if (!instances || !instances->isAlive(slot))
            return;
        scene.forEachActiveView([object](auto& view) { view.unregisterObject(object); });
        instances->cancelFadeRetirement(object);
        const auto binding = instances->takeResources(object);
        instances->freeObject(object);
        if (!binding)
            return;
        if (auto* materials = context.globalRegistry().find<MaterialResources>())
        {
            materials->releaseFromInstance(binding->material);
        }
        if (auto* meshes = context.globalRegistry().find<MeshResources>())
            meshes->releaseFromInstance(binding->mesh);
    }

    void detail::makeMeshInstanceVisible(
        void* server_state,
        RenderSceneId scene_id,
        std::uint32_t view_index,
        RenderObjectHandle object
    ) noexcept
    {
        auto* scene = lookupScene(server_state, scene_id);
        auto* instances = scene ? scene->sceneRegistry().find<InstanceResources>() : nullptr;
        const auto slot = resolveInstanceSlot(instances, object);
        if (!scene || !instances || !instances->isAlive(slot))
            return;
        const auto bucket = instances->cullMetaAt(slot).bucket_id;
        scene->forEachActiveView([view_index, object, bucket](auto& view) {
            if (view.handle.index == view_index)
                view.registerObject(object, bucket);
        }
        );
    }

    void detail::setMeshInstanceVisibility(
        void* server_state,
        RenderSceneId scene_id,
        RenderObjectHandle object,
        bool visible
    ) noexcept
    {
        auto* scene = lookupScene(server_state, scene_id);
        auto* instances = scene ? scene->sceneRegistry().find<InstanceResources>() : nullptr;
        const auto slot = resolveInstanceSlot(instances, object);
        if (!scene || !instances || !instances->isAlive(slot))
            return;
        const auto bucket = instances->cullMetaAt(slot).bucket_id;
        auto flags = instances->propertyAt(slot).flags;
        if (visible)
            flags |= kInstanceFlagVisible;
        else
            flags &= ~kInstanceFlagVisible;
        instances->setInstanceFlags(slot, flags);
        scene->forEachActiveView([object, bucket, visible](auto& view) {
            if (visible)
                view.registerObject(object, bucket);
            else
                view.unregisterObject(object);
        }
        );
    }

    // ── Instance lifecycle ───────────────────────────────────────────────
    void handleAddMeshInstance(GeneralRenderServer::Dispatcher::Ctx& ctx, const AddMeshInstancePayload& p)
    {
        MeshInstanceCreateStatus status = MeshInstanceCreateStatus::Unknown;
        const RenderObjectHandle object = detail::createMeshInstance(
            ctx.user_state,
            p.scene_id,
            p.mesh,
            p.material,
            p.transform,
            p.flags & ~kInstanceInternalFlagClusterOwned,
            p.geometry_kind,
            p.pass_mask,
            p.user_meta_index,
            false,
            status,
            p.transition_milliseconds,
            p.transition_seed
        );
        replyToCurrent<AddMeshInstancePayload>(ctx, MeshInstanceSlotReply{object, status});
    }

    void handleRemoveMeshInstance(GeneralRenderServer::Dispatcher::Ctx& ctx, const RemoveMeshInstancePayload& p)
    {
        detail::destroyMeshInstance(ctx.user_state, p.scene_id, p.object);
    }

    void handleRetireMeshInstance(GeneralRenderServer::Dispatcher::Ctx& ctx, const RetireMeshInstancePayload& p)
    {
        auto* scene = lookupScene(ctx.user_state, p.scene_id);
        auto* instances = scene ? scene->sceneRegistry().find<InstanceResources>() : nullptr;
        const float duration = static_cast<float>(p.transition_milliseconds) / 1000.0f;
        if (!scene || !instances ||
            !instances->beginFadeRetirement(p.object, scene->sceneTime(), duration, p.transition_seed))
        {
            detail::destroyMeshInstance(ctx.user_state, p.scene_id, p.object);
        }
    }

    void handleMeshStackStats(GeneralRenderServer::Dispatcher::Ctx& ctx, const MeshStackStatsPayload& payload)
    {
        auto* scene = lookupScene(ctx.user_state, payload.scene_id);
        const auto* instances = scene ? scene->sceneRegistry().find<InstanceResources>() : nullptr;
        auto* render_context = lookupRenderContext(ctx.user_state);
        const auto* meshes = render_context ? render_context->globalRegistry().find<MeshResources>() : nullptr;
        MeshStackStatsReply reply{};
        if (instances)
        {
            reply.alive_instances = instances->aliveCount();
            reply.cluster_owned_instances = instances->flagBitCount(31u);
            reply.transitioning_instances = instances->fadeRetirementCount();
            reply.resource_bound_instances = instances->resourceBindingCount();
            reply.transparent_hard_cuts = instances->transparentHardCutCount();
        }
        if (meshes)
        {
            const auto vbo = meshes->vboTelemetry();
            const auto ibo = meshes->iboTelemetry();
            reply.vbo_segment_count = vbo.segment_count;
            reply.ibo_segment_count = ibo.segment_count;
            reply.vbo_growth_count = vbo.growth_count;
            reply.ibo_growth_count = ibo.growth_count;
            reply.vbo_used_bytes = vbo.used_bytes;
            reply.vbo_free_bytes = vbo.free_bytes;
            reply.vbo_largest_free_block = vbo.largest_free_block;
            reply.ibo_used_bytes = ibo.used_bytes;
            reply.ibo_free_bytes = ibo.free_bytes;
            reply.ibo_largest_free_block = ibo.largest_free_block;
            reply.vbo_fragmentation = vbo.fragmentation;
            reply.ibo_fragmentation = ibo.fragmentation;
        }
        replyToCurrent<MeshStackStatsPayload>(ctx, reply);
    }

    void handleMakeInstanceVisibleForView(
        GeneralRenderServer::Dispatcher::Ctx& ctx,
        const MakeInstanceVisibleForViewPayload& p
    )
    {
        RenderScene* sc = nullptr;
        auto* inst = resolveInstances(ctx, p.scene_id, sc);
        if (!inst)
            return;
        auto* view = sc->getView(p.view);
        if (!view)
            return;
        const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
        if (!inst->isAlive(slot))
            return;

        // Derive bucket_id from the already-written cull meta.
        const auto& meta = inst->cullMetaAt(slot);
        view->registerObject(p.object, meta.bucket_id);
    }

    void handleHideInstanceFromView(GeneralRenderServer::Dispatcher::Ctx& ctx, const HideInstanceFromViewPayload& p)
    {
        RenderScene* sc = nullptr;
        auto* inst = resolveInstances(ctx, p.scene_id, sc);
        if (!inst)
            return;
        const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
        if (!inst->isAlive(slot))
            return;
        if (auto* view = sc->getView(p.view))
            view->unregisterObject(p.object);
    }

    void handleUpdateInstanceFlags(GeneralRenderServer::Dispatcher::Ctx& ctx, const UpdateInstanceFlagsPayload& p)
    {
        RenderScene* sc = nullptr;
        auto* inst = resolveInstances(ctx, p.scene_id, sc);
        if (!inst)
            return;
        const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
        if (!inst->isAlive(slot))
            return;
        // flags 收口写点:走 setInstanceFlags 维护逐位存活计数
        //(Highlight 等 feature 依赖它做整链跳过判定)。
        const auto internal_flags = inst->propertyAt(slot).flags & kInstanceInternalFlagClusterOwned;
        inst->setInstanceFlags(slot, (p.flags & ~kInstanceInternalFlagClusterOwned) | internal_flags);
    }

    void handleUpdateInstanceRenderState(
        GeneralRenderServer::Dispatcher::Ctx& ctx,
        const UpdateInstanceRenderStatePayload& p
    )
    {
        RenderScene* sc = nullptr;
        auto* inst = resolveInstances(ctx, p.scene_id, sc);
        if (!inst)
            return;
        const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
        if (!inst->isAlive(slot))
            return;
        inst->setRenderState(slot, p.geometry_kind, p.pass_mask);
    }

    void handleUpdateInstanceUserMeta(GeneralRenderServer::Dispatcher::Ctx& ctx, const UpdateInstanceUserMetaPayload& p)
    {
        RenderScene* sc = nullptr;
        auto* inst = resolveInstances(ctx, p.scene_id, sc);
        if (!inst)
            return;
        const InstanceSlot slot = resolveInstanceSlot(inst, p.object);
        if (!inst->isAlive(slot))
            return;
        auto& prop = inst->propertyAt(slot);
        prop.user_meta_index = p.user_meta_index;
        inst->markPropertyDirty(slot);
    }

    // ── Per-frame transform batch (each entry self-routes by scene_id, G-04) ──
    void handleTransformBatch(GeneralRenderServer::Dispatcher::Ctx& ctx, std::span<const TransformWriteEntry> entries)
    {
        // No SetActiveScene dependency: resolve each entry's OWN scene, so batches
        // interleaved from different scenes (editor main + preview) never cross.
        // Batches are typically single-scene (the mesh bridge sends one entry per
        // instance), so the per-entry resolve is cheap.
        for (const auto& e : entries)
        {
            RenderScene* sc = nullptr;
            auto* inst = resolveInstances(ctx, e.scene_id, sc);
            if (!inst)
                continue;
            const InstanceSlot slot = resolveInstanceSlot(inst, e.object);
            if (!inst->isAlive(slot))
                continue;
            inst->writeTransform(slot, e.transform);
        }
    }

    // ── Mesh DATA upload/destroy (global arena; no scene_id) ───────────────
    void handleUploadMesh(GeneralRenderServer::Dispatcher::Ctx& ctx, const UploadMeshPayload& p)
    {
        // Resolve the shared-owned rdesc::Mesh (the owner travels into the async
        // worker to keep vdata/idata alive past this handler — see serverUploadMesh).
        auto view = resolveExternalDataView(ctx.program, p.mesh_desc);
        const auto* mesh = reinterpret_cast<const lux::rdesc::Mesh*>(view.bytes.data());
        // Success replies are DEFERRED by the shared upload worker (MeshUploadedReply,
        // request-id correlated). Only a synchronous allocate failure replies here.
        lux::render::CapacityShortfallWire shortfall{};
        if (!serverUploadMesh(
                ctx.user_state,
                *mesh,
                p.layout_id,
                ctx.currentRequestId(),
                std::move(view.owner),
                &shortfall))
        {
            replyToCurrent<UploadMeshPayload>(ctx, MeshUploadedReply{RMeshHandle{}, 1u, shortfall});
        }
    }

    void handleDestroyMesh(GeneralRenderServer::Dispatcher::Ctx& ctx, const DestroyMeshPayload& p)
    {
        serverDestroyMesh(ctx.user_state, p.handle);
    }

} // namespace lux::render
