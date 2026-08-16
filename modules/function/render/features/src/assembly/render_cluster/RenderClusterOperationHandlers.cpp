#include <lux/engine/render/comm/server/RenderServer.hpp>
#include <lux/engine/function/render/client/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/render/renderer/features/render_cluster/RenderClusterResources.hpp>
#include <lux/engine/render/renderer/features/meshstack/MeshInstanceAssembly.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace lux::render
{
    RenderScene* lookupScene(void* user_state, RenderSceneId scene_id);

    namespace
    {
        struct CullValidationCounts final
        {
            std::uint32_t visible_flags{0u};
            std::uint32_t gbuffer_passes{0u};
            std::uint32_t geometries{0u};
            std::uint32_t lods{0u};
            std::uint32_t mdcs{0u};
            std::uint32_t frustum{0u};
            std::uint32_t non_white_instances{0u};
            std::uint32_t rgba8_xor{0u};
        };

        [[nodiscard]] CullValidationCounts validateViewCullInputs(
            RenderScene& scene,
            const RenderClusterResources& clusters) noexcept
        {
            CullValidationCounts result{};
            const auto* instances = scene.sceneRegistry().find<
                InstanceResources>();
            const auto* cameras = scene.sceneRegistry().find<
                ViewCameraResource>();
            if (instances == nullptr || cameras == nullptr)
                return result;

            const ViewFrameData* camera = nullptr;
            scene.forEachActiveView([&](const View& view)
            {
                if (camera == nullptr)
                    camera = cameras->find(view.handle.index);
            });
            if (camera == nullptr)
                return result;

            const Frustum frustum = Frustum::fromViewProj(
                viewRelativeViewProjection(*camera));
            const auto& mdc_entries = instances->mdcTable().entries();
            const float page_size = instances->spatialTileSize();
            std::vector<std::uint32_t> candidate_slots;
            candidate_slots.reserve(
                clusters.visibleInstanceCount() + instances->dynamicCount());
            candidate_slots.insert(
                candidate_slots.end(),
                instances->denseDynamicSlots().begin(),
                instances->denseDynamicSlots().end());
            clusters.forEachVisibleObject([&](RenderObjectHandle object)
            {
                const auto slot = instances->resolveSlot(object);
                if (instances->isAlive(slot))
                    candidate_slots.push_back(slot.index);
            });
            for (const std::uint32_t slot_index : candidate_slots)
            {
                const InstanceSlot slot{slot_index};
                const auto& property = instances->propertyAt(slot);
                const auto& cull = instances->cullMetaAt(slot);
                result.rgba8_xor ^= property.rgba8;
                if (property.rgba8 != 0xffffffffu)
                    ++result.non_white_instances;
                if ((property.flags & kInstanceFlagVisible) == 0u)
                    continue;
                ++result.visible_flags;
                if ((property.pass_and_geometry &
                     static_cast<std::uint32_t>(eGBuffer)) == 0u)
                {
                    continue;
                }
                ++result.gbuffer_passes;
                const auto geometry =
                    (property.pass_and_geometry >> 16u) & 0xffu;
                if (geometry != static_cast<std::uint32_t>(
                        EGeometryKind::StaticMesh) &&
                    geometry != static_cast<std::uint32_t>(
                        EGeometryKind::SkinnedMesh))
                {
                    continue;
                }
                ++result.geometries;
                if (cull.bsphere[3] < 0.0f || cull.lod_count == 0u)
                    continue;
                ++result.lods;
                const auto mdc = cull.lod_mdc[0];
                if (mdc >= mdc_entries.size() ||
                    mdc_entries[mdc].capacity == 0u)
                {
                    continue;
                }
                ++result.mdcs;

                Eigen::Vector3f center{};
                for (std::size_t axis = 0u; axis < 3u; ++axis)
                {
                    center[axis] = static_cast<float>(
                        cull.bsphere_page[axis] -
                        camera->render_origin.page_delta[axis]) * page_size +
                        cull.bsphere[axis] -
                        camera->render_origin.local[axis];
                }
                bool inside = true;
                for (const auto& plane : frustum.planes)
                {
                    if (plane.normal.dot(center) + plane.d <
                        -cull.bsphere[3])
                    {
                        inside = false;
                        break;
                    }
                }
                if (inside)
                    ++result.frustum;
            }
            return result;
        }

        void applyHierarchyVisibility(
            void* server_state,
            RenderScene& scene,
            RenderClusterResources& resources,
            RenderClusterWireId family)
        {
            auto* instances = scene.sceneRegistry().find<InstanceResources>();
            if (!instances)
                return;
            for (const auto change : resources.reconcileHierarchy(
                     family,
                     true,
                     scene.sceneTime(),
                     resources.transitionDurationSeconds()))
            {
                const auto* cluster = resources.find(change.id);
                if (!cluster)
                    continue;
                for (std::size_t index = 0u;
                     index < cluster->objects.size(); ++index)
                {
                    const auto object = cluster->objects[index];
                    const auto slot = instances->resolveSlot(object);
                    if (instances->isAlive(slot))
                    {
                        auto property = instances->propertyAt(slot);
                        property.transition_start_time =
                            change.transition_start_time;
                        property.transition_duration =
                            change.transition_duration;
                        property.transition_seed =
                            RenderClusterResources::transitionSeed(
                                cluster->instances[index].stable_pick_id,
                                cluster->header.id,
                                index);
                        property.transition_flags =
                            change.transition == RenderClusterResources::
                                    ETransitionAction::NONE
                            ? 0u
                            : 1u | (change.transition ==
                                          RenderClusterResources::
                                              ETransitionAction::FADE_OUT
                                      ? 2u
                                      : 0u);
                        instances->writeProperty(slot, property);
                    }
                    detail::setMeshInstanceVisibility(
                        server_state,
                        scene.sceneId(),
                        object,
                        change.visible);
                }
            }
        }
    } // namespace

    void handleRenderClusterUpload(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const UploadRenderClusterPayload& payload)
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        auto* resources = scene
            ? scene->sceneRegistry().find<RenderClusterResources>()
            : nullptr;
        const auto blob = resolveExternalData(
            context.program, payload.instances);
        const auto required_bytes = static_cast<std::uint64_t>(
            payload.instance_count) * sizeof(RenderClusterWireInstance);
        std::uint32_t status = 1u;
        if (resources && required_bytes == blob.size() &&
            required_bytes <= std::numeric_limits<std::size_t>::max() &&
            (blob.empty() || reinterpret_cast<std::uintptr_t>(blob.data()) %
                alignof(RenderClusterWireInstance) == 0u))
        {
            const auto instances = std::span<const
                RenderClusterWireInstance>{
                reinterpret_cast<const RenderClusterWireInstance*>(
                    blob.data()),
                payload.instance_count};
            if (!resources->accepts(payload.id, payload.revision))
            {
                status = 0u;
            }
            else
            {
                if (!resources->validatesUpsert(
                        payload,
                        instances,
                        instances.size(),
                        instances.size()))
                {
                    status = 2u;
                    replyToCurrent<UploadRenderClusterPayload>(
                        context,
                        RenderClusterUploadedReply{
                            payload.id,
                            payload.instance_count,
                            status});
                    return;
                }
                RenderClusterWireId previous_parent{};
                std::vector<RenderObjectHandle> previous_objects;
                if (const auto* previous = resources->find(payload.id))
                {
                    previous_parent = previous->header.parent;
                    previous_objects = previous->objects;
                }
                const auto reused_count = std::min(
                    previous_objects.size(),
                    instances.size());
                std::vector<RenderObjectHandle> created;
                std::vector<std::uint32_t> created_tokens;
                created.reserve(instances.size());
                created_tokens.reserve(instances.size());
                created.insert(
                    created.end(),
                    previous_objects.begin(),
                    previous_objects.begin() + reused_count);

                const auto rollback_new_objects = [&]() noexcept
                {
                    for (std::size_t index = reused_count;
                         index < created.size();
                         ++index)
                    {
                        detail::destroyMeshInstance(
                            context.user_state,
                            payload.scene_id,
                            created[index]);
                    }
                    created.resize(reused_count);
                };
                const auto rollback_tokens = [&]() noexcept
                {
                    for (const auto token : created_tokens)
                        resources->cancelPickToken(token);
                    created_tokens.clear();
                };

                for (const auto& instance : instances)
                {
                    const auto pick_token = instance.stable_pick_id == 0u
                        ? 0u
                        : resources->allocatePickToken(
                              instance.stable_pick_id);
                    if (instance.stable_pick_id != 0u && pick_token == 0u)
                    {
                        rollback_new_objects();
                        rollback_tokens();
                        status = 3u;
                        break;
                    }
                    created_tokens.push_back(pick_token);
                }
                for (std::size_t index = reused_count;
                     status != 3u && index < instances.size();
                     ++index)
                {
                    const auto& instance = instances[index];
                    MeshInstanceCreateStatus create_status{
                        MeshInstanceCreateStatus::Unknown};
                    const auto object = detail::createMeshInstance(
                        context.user_state,
                        payload.scene_id,
                        instance.mesh,
                        instance.material,
                        instance.transform,
                        (instance.flags & ~kInstanceFlagVisible) |
                            kInstanceInternalFlagClusterOwned,
                        EGeometryKind::StaticMesh,
                        kPassMaskOpaqueDefault,
                        created_tokens[index],
                        false,
                        create_status);
                    if (!object || create_status !=
                            MeshInstanceCreateStatus::Ok)
                    {
                        rollback_new_objects();
                        rollback_tokens();
                        status = create_status ==
                                MeshInstanceCreateStatus::CapacityExhausted
                            ? 3u
                            : 2u;
                        break;
                    }
                    auto* instance_resources = scene->sceneRegistry().find<
                        InstanceResources>();
                    const auto instance_slot = instance_resources
                        ? instance_resources->resolveSlot(object)
                        : InstanceSlot::invalid();
                    if (!instance_resources ||
                        !instance_resources->isAlive(instance_slot))
                    {
                        detail::destroyMeshInstance(
                            context.user_state,
                            payload.scene_id,
                            object);
                        rollback_new_objects();
                        rollback_tokens();
                        status = 2u;
                        break;
                    }
                    auto property =
                        instance_resources->propertyAt(instance_slot);
                    property.rgba8 = instance.rgba8;
                    instance_resources->writeProperty(
                        instance_slot,
                        property);
                    created.push_back(object);
                }
                if (status != 3u && created.size() == instances.size() &&
                    created_tokens.size() == instances.size())
                {
                    std::vector<detail::MeshInstanceRevision> revisions;
                    revisions.reserve(reused_count);
                    for (std::size_t index = 0u;
                         index < reused_count;
                         ++index)
                    {
                        const auto& instance = instances[index];
                        revisions.push_back(detail::MeshInstanceRevision{
                            created[index],
                            instance.mesh,
                            instance.material,
                            instance.transform,
                            (instance.flags & ~kInstanceFlagVisible) |
                                kInstanceInternalFlagClusterOwned,
                            EGeometryKind::StaticMesh,
                            kPassMaskOpaqueDefault,
                            created_tokens[index],
                            instance.rgba8});
                    }
                    MeshInstanceCreateStatus reconfigure_status{
                        MeshInstanceCreateStatus::Ok};
                    if (!detail::reconfigureMeshInstances(
                            context.user_state,
                            payload.scene_id,
                            revisions,
                            reconfigure_status))
                    {
                        rollback_new_objects();
                        rollback_tokens();
                        status = reconfigure_status ==
                                MeshInstanceCreateStatus::CapacityExhausted
                            ? 3u
                            : 2u;
                    }
                    else if (resources->upsert(
                                 payload,
                                 instances,
                                 created,
                                 created_tokens))
                    {
                        const auto family = payload.parent.valid()
                            ? payload.parent
                            : payload.id;
                        applyHierarchyVisibility(
                            context.user_state,
                            *scene,
                            *resources,
                            family);
                        if (previous_parent.valid() &&
                            previous_parent != family)
                        {
                            applyHierarchyVisibility(
                                context.user_state,
                                *scene,
                                *resources,
                                previous_parent);
                        }
                        for (std::size_t index = reused_count;
                             index < previous_objects.size();
                             ++index)
                        {
                            detail::destroyMeshInstance(
                                context.user_state,
                                payload.scene_id,
                                previous_objects[index]);
                        }
                        status = 0u;
                    }
                    else
                    {
                        renderFatal(
                            "Validated RenderCluster revision failed during "
                            "safe-point commit");
                    }
                }
            }
        }
        replyToCurrent<UploadRenderClusterPayload>(
            context,
            RenderClusterUploadedReply{
                payload.id, payload.instance_count, status});
    }

    void handleRenderClusterRemove(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const RemoveRenderClusterPayload& payload)
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        auto* resources = scene
            ? scene->sceneRegistry().find<RenderClusterResources>()
            : nullptr;
        if (!resources)
        {
            replyToCurrent<RemoveRenderClusterPayload>(
                context,
                RenderClusterRemovedReply{
                    payload.id, payload.revision, 1u, 0u});
            return;
        }
        if (!resources->accepts(payload.id, payload.revision))
        {
            replyToCurrent<RemoveRenderClusterPayload>(
                context,
                RenderClusterRemovedReply{
                    payload.id, payload.revision, 0u, 0u});
            return;
        }
        std::vector<RenderObjectHandle> retired;
        UploadRenderClusterPayload previous_header{};
        if (const auto* previous = resources->find(payload.id))
        {
            retired = previous->objects;
            previous_header = previous->header;
        }
        if (!resources->remove(payload.id, payload.revision))
        {
            replyToCurrent<RemoveRenderClusterPayload>(
                context,
                RenderClusterRemovedReply{
                    payload.id, payload.revision, 2u, 0u});
            return;
        }
        if (previous_header.parent.valid())
        {
            applyHierarchyVisibility(
                context.user_state,
                *scene,
                *resources,
                previous_header.parent);
        }
        if (previous_header.child_count != 0u)
        {
            applyHierarchyVisibility(
                context.user_state,
                *scene,
                *resources,
                previous_header.id);
        }
        for (const auto object : retired)
        {
            detail::destroyMeshInstance(
                context.user_state,
                payload.scene_id,
                object);
        }
        replyToCurrent<RemoveRenderClusterPayload>(
            context,
            RenderClusterRemovedReply{
                payload.id, payload.revision, 0u, 0u});
    }

    void handleRenderClusterStats(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const QueryRenderClusterStatsPayload& payload)
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        auto* resources = scene
            ? scene->sceneRegistry().find<RenderClusterResources>()
            : nullptr;
        const auto clusters = resources
            ? std::min<std::size_t>(
                  resources->clusterCount(),
                  std::numeric_limits<std::uint32_t>::max())
            : 0u;
        const auto instances = resources
            ? std::min<std::size_t>(
                  resources->instanceCount(),
                  std::numeric_limits<std::uint32_t>::max())
            : 0u;
        const auto visible_clusters = resources
            ? std::min<std::size_t>(
                  resources->visibleClusterCount(),
                  std::numeric_limits<std::uint32_t>::max())
            : 0u;
        const auto visible_instances = resources
            ? std::min<std::size_t>(
                  resources->visibleInstanceCount(),
                  std::numeric_limits<std::uint32_t>::max())
            : 0u;
        const auto candidate_count = resources
            ? resources->latestGpuCandidateCount()
            : 0u;
        const auto candidate_valid = resources &&
            resources->hasValidGpuCandidateDispatch();
        const auto cull_validation = scene && resources
            ? validateViewCullInputs(*scene, *resources)
            : CullValidationCounts{};
        const auto mip_feedback = scene
            ? scene->renderContext().globalRegistry()
                .must<TextureResources>().mipFeedbackSnapshot()
            : TextureMipFeedbackSnapshot{};
        const auto cpu_memory = resources
            ? resources->cpuMemorySnapshot()
            : RenderClusterResources::CpuMemorySnapshot{};
        replyToCurrent<QueryRenderClusterStatsPayload>(
            context,
            RenderClusterStatsReply{
                static_cast<std::uint32_t>(clusters),
                static_cast<std::uint32_t>(instances),
                static_cast<std::uint32_t>(visible_clusters),
                static_cast<std::uint32_t>(visible_instances),
                candidate_count,
                candidate_valid ? 1u : 0u,
                mip_feedback.sampled_texture_count,
                mip_feedback.minimum_wanted_mip,
                mip_feedback.valid,
                resources
                    ? resources->latestGpuCandidateGroupCount()
                    : 0u,
                mip_feedback.aggregation_fallback_count,
                cull_validation.visible_flags,
                cull_validation.gbuffer_passes,
                cull_validation.geometries,
                cull_validation.lods,
                cull_validation.mdcs,
                cull_validation.frustum,
                cull_validation.non_white_instances,
                cull_validation.rgba8_xor,
                mip_feedback.full_resident_bytes,
                mip_feedback.target_resident_bytes,
                mip_feedback.actual_resident_bytes,
                resources
                    ? resources->latestGpuCandidateRequestedCount()
                    : 0u,
                resources
                    ? resources->latestGpuCandidateOverflowCount()
                    : 0u,
                cpu_memory.capacity_bytes,
                cpu_memory.allocation_count});
    }

    void handleRenderClusterPickRequest(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const RequestRenderClusterPickPayload& payload)
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        auto* resources = scene
            ? scene->sceneRegistry().find<RenderClusterResources>()
            : nullptr;
        if (resources)
        {
            const ViewHandle view{
                payload.view_index, payload.view_generation};
            if (scene->getView(view))
                resources->requestPick(payload);
            else
                resources->failPick(payload, ERenderPickStatus::STALE);
        }
    }

    void handleRenderClusterPickResult(
        GeneralRenderServer::Dispatcher::Ctx& context,
        const QueryRenderClusterPickPayload& payload)
    {
        auto* scene = lookupScene(context.user_state, payload.scene_id);
        auto* resources = scene
            ? scene->sceneRegistry().find<RenderClusterResources>()
            : nullptr;
        RenderClusterPickReply reply{};
        if (resources)
        {
            reply = resources->pickResult(payload.request_generation);
        }
        else
        {
            reply.request_generation = payload.request_generation;
            reply.status = ERenderPickStatus::FAILED;
        }
        replyToCurrent<QueryRenderClusterPickPayload>(context, reply);
    }
} // namespace lux::render
