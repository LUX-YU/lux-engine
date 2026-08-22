#include <lux/engine/runtime/render/scene/ClassicMeshBatchRenderSubsystem.hpp>
#include <lux/engine/runtime/render/scene/SceneGeometryPrepareService.hpp>
#include <lux/engine/runtime/render/scene/detail/SceneContentRenderContracts.hpp>
#include <lux/engine/runtime/scene/SceneAsyncContext.hpp>

#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/components/PersistentEntityIdComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/render/RenderResourceEvents.hpp>
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include <lux/engine/ecs/render/TrackedRenderRequest.hpp>
#include <lux/engine/ecs/render/RenderViewUtil.hpp>
#include <lux/engine/ecs/render/components/3d/ClassicMeshBatchComponent.hpp>
#include <lux/engine/ecs/render/components/3d/VisualLodNodeComponent.hpp>
#include <lux/engine/ecs/render/subsystems/ResidencySubsystem.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/genops/RenderClusterOperation.ops.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/function/render/standard/content/ClassicMeshBatch.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>

#include <stdexec/execution.hpp>

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::runtime
{
    namespace
    {
        constexpr std::size_t kRowsPerUpdate = 4096u;

        [[nodiscard]] lux::render::RenderClusterWireId uuidWireId(
            const lux::ecs::PersistentEntityId& id) noexcept
        {
            lux::render::RenderClusterWireId result{};
            if (id.empty())
                return result;
            const auto bytes = id.value().as_bytes();
            static_assert(sizeof(result.bytes) == 16u);
            std::memcpy(result.bytes, bytes.data(), sizeof(result.bytes));
            return result;
        }

        [[nodiscard]] lux::render::RenderClusterWireId entityWireId(
            lux::ecs::Registry& registry,
            lux::ecs::Entity entity) noexcept
        {
            if (const auto* persistent = registry.try_get<
                    lux::ecs::PersistentEntityIdComponent>(entity))
            {
                return uuidWireId(persistent->id());
            }

            lux::render::RenderClusterWireId result{};
            const auto raw = static_cast<std::uint64_t>(
                entt::to_integral(entity));
            constexpr std::uint64_t salt = 0x434d4252434c5558ull;
            std::memcpy(result.bytes, &raw, sizeof(raw));
            std::memcpy(result.bytes + sizeof(raw), &salt, sizeof(salt));
            return result;
        }

        [[nodiscard]] std::uint32_t renderFlags(
            std::uint32_t source) noexcept
        {
            using Flag = lux::classic_mesh::EClassicMeshInstanceFlag;
            std::uint32_t result = 0u;
            if ((source & static_cast<std::uint32_t>(Flag::VISIBLE)) != 0u)
                result |= lux::render::kInstanceFlagVisible;
            if ((source & static_cast<std::uint32_t>(Flag::CAST_SHADOW)) != 0u)
                result |= lux::render::kInstanceFlagCastShadow;
            if ((source & static_cast<std::uint32_t>(
                    Flag::RECEIVE_SHADOW)) != 0u)
            {
                result |= lux::render::kInstanceFlagReceiveShadow;
            }
            return result;
        }

        [[nodiscard]] bool sameNeed(
            const lux::asset::asset_id_t& left_id,
            lux::ecs::EResourceDomain left_domain,
            const lux::asset::asset_id_t& right_id,
            lux::ecs::EResourceDomain right_domain) noexcept
        {
            return left_id == right_id && left_domain == right_domain;
        }
    } // namespace

    struct ClassicMeshBatchObservedCommand final
    {
        using Producer = ClassicMeshBatchRenderSubsystem;

        lux::ecs::Entity entity{entt::null};
        bool topology{false};

        [[nodiscard]] std::size_t registryPublicationBytes() const noexcept
        {
            return 0u;
        }
        void prepareRegistryPublication(
            lux::ecs::Registry&) const noexcept
        {}

        void apply(
            lux::ecs::Registry&,
            ClassicMeshBatchRenderSubsystem& owner) const noexcept
        {
            owner.applyObservedChange(entity, topology);
        }
    };
    static_assert(std::is_trivially_copyable_v<
        ClassicMeshBatchObservedCommand>);

    struct ClassicMeshBatchRenderSubsystem::Impl final
    {
        struct AssetNeed final
        {
            lux::asset::asset_id_t id{};
            lux::ecs::EResourceDomain domain{
                lux::ecs::EResourceDomain::MESH};
            lux::asset::AssetRef reference;
            lux::ecs::ResidencyCallbacks::Ticket wait;
            std::uint64_t handle_bits{0u};
            bool ready{false};
            bool failed{false};
        };

        struct Pending final
        {
            std::uint64_t desired_generation{0u};
            std::uint64_t owner_generation{0u};
            std::uint64_t revision{0u};
            lux::render::RenderClusterWireId id{};
            lux::render::UploadRenderClusterPayload payload{};
            entity_scene::ContentBlobLease blob;
            std::shared_ptr<lux::classic_mesh::ClassicMeshBatchBlobV1>
                decoded;
            std::shared_ptr<std::vector<
                lux::render::RenderClusterWireInstance>> wire;
            std::vector<AssetNeed> assets;
            std::size_t next_row{0u};
            bool upload_submitted{false};
        };

        struct Preparation final
        {
            std::uint64_t desired_generation{0u};
            std::uint64_t owner_generation{0u};
            entity_scene::ContentBlobLease blob;
            bool in_flight{false};
        };

        struct Active final
        {
            std::uint64_t revision{0u};
            std::uint32_t item_count{0u};
            lux::render::RenderClusterWireId id{};
            entity_scene::ContentBlobLease blob;
            std::vector<lux::asset::AssetRef> assets;
        };

        struct Entry final
        {
            std::uint64_t owner_generation{0u};
            std::uint64_t desired_generation{0u};
            ESceneContentRenderState state{
                ESceneContentRenderState::WAITING_CONTENT};
            ESceneContentRenderFailure failure{
                ESceneContentRenderFailure::NONE};
            std::optional<Preparation> preparation;
            std::optional<Pending> pending;
            std::optional<Active> active;
        };

        struct CallbackControl final
        {
            ClassicMeshBatchRenderSubsystem* owner{nullptr};
            lux::render::RenderControlSession* control{nullptr};
            lux::render::RenderSceneId scene{};
            lux::render::RenderClusterOperationIds operations{};
        };

        struct UploadContext final
        {
            lux::ecs::Entity entity{entt::null};
            std::uint64_t owner_generation{0u};
            std::uint64_t revision{0u};
            lux::render::RenderClusterWireId id{};
        };

        Impl(
            entity_scene::ContentBlobClient blob_client,
            lux::ecs::ResidencyCallbacks callbacks,
            lux::asset::AssetManager& asset_manager,
            ClassicMeshPrepareClient preparation_client,
            SceneAsyncContext& async) noexcept
            : blobs(std::move(blob_client)),
              residency(std::move(callbacks)),
              assets(&asset_manager),
              preparation_client(std::move(preparation_client)),
              async_scope(&async.scope()),
              callbacks(std::make_shared<CallbackControl>())
        {}

        [[nodiscard]] std::uint64_t nextRevision() noexcept
        {
            return revisions.next();
        }

        void enqueue(lux::ecs::Entity entity, bool topology) noexcept
        {
            (void)commands.push(ClassicMeshBatchObservedCommand{
                entity, topology});
        }

        void onTopology(
            lux::ecs::RegistryBase&,
            entt::entity entity) noexcept
        {
            enqueue(entity, true);
        }

        void onPresentationFact(
            lux::ecs::RegistryBase&,
            entt::entity entity) noexcept
        {
            enqueue(entity, false);
        }

        void attach(
            lux::ecs::Registry& value,
            lux::ecs::EcsCommandWriter writer)
        {
            registry = &value;
            commands = writer;
            topology_dirty = true;
            changes.attach(
                value,
                [this](lux::ecs::Entity entity)
                {
                    enqueue(entity, false);
                });
            leaves.attach(
                value,
                [this](lux::ecs::Entity entity)
                {
                    enqueue(entity, false);
                });
            lod_constructed = value.on_construct<
                lux::ecs::VisualLodNodeComponent>()
                .connect<&Impl::onTopology>(*this);
            lod_updated = value.on_update<
                lux::ecs::VisualLodNodeComponent>()
                .connect<&Impl::onTopology>(*this);
            lod_destroyed = value.on_destroy<
                lux::ecs::VisualLodNodeComponent>()
                .connect<&Impl::onTopology>(*this);
            parent_constructed = value.on_construct<
                lux::ecs::VisualLodParentComponent>()
                .connect<&Impl::onTopology>(*this);
            parent_updated = value.on_update<
                lux::ecs::VisualLodParentComponent>()
                .connect<&Impl::onTopology>(*this);
            parent_destroyed = value.on_destroy<
                lux::ecs::VisualLodParentComponent>()
                .connect<&Impl::onTopology>(*this);
            persistent_constructed = value.on_construct<
                lux::ecs::PersistentEntityIdComponent>()
                .connect<&Impl::onTopology>(*this);
            persistent_destroyed = value.on_destroy<
                lux::ecs::PersistentEntityIdComponent>()
                .connect<&Impl::onTopology>(*this);
            transform_updated = value.on_update<
                lux::ecs::ResolvedTransform3DComponent>()
                .connect<&Impl::onPresentationFact>(*this);
        }

        void detach() noexcept
        {
            changes.detach();
            leaves.detach();
            lod_constructed.release();
            lod_updated.release();
            lod_destroyed.release();
            parent_constructed.release();
            parent_updated.release();
            parent_destroyed.release();
            persistent_constructed.release();
            persistent_destroyed.release();
            transform_updated.release();
            registry = nullptr;
            commands = {};
            children_by_parent.clear();
        }

        void prepare(lux::ecs::RenderSubsystemContext& context) noexcept
        {
            callbacks->control = &context.render().control();
            callbacks->scene = context.render().scene();
            callbacks->operations = context.render().features().ops<
                lux::render::RenderClusterOperationIds>("RenderCluster");
        }

        void sendRemove(
            lux::render::RenderClusterWireId id,
            std::uint64_t revision,
            bool compensation = false) noexcept
        {
            if (!id.valid() || !callbacks->control ||
                !callbacks->operations.valid() || callbacks->scene.isNull())
            {
                return;
            }
            (void)lux::render::RenderClusterControlClient{
                *callbacks->control, callbacks->operations}.remove(
                    lux::render::RemoveRenderClusterPayload{
                        callbacks->scene, id, revision});
            if (compensation)
                ++metrics.compensated_removals;
        }

        void retire(lux::ecs::Entity entity) noexcept
        {
            const auto found = entries.find(entity);
            if (found == entries.end())
                return;
            auto& entry = found->second;
            lux::render::RenderClusterWireId id{};
            if (entry.pending)
                id = entry.pending->id;
            else if (entry.active)
                id = entry.active->id;
            else if (registry)
                id = entityWireId(*registry, entity);
            if (id.valid() && (entry.active ||
                    (entry.pending && entry.pending->upload_submitted)))
            {
                sendRemove(id, nextRevision());
            }
            if (entry.pending && entry.pending->upload_submitted)
                (void)uploads.abandon(entry.pending->revision);
            entries.erase(found);
            dirty.erase(entity);
        }

        void observed(lux::ecs::Entity entity, bool topology) noexcept
        {
            if (!registry || !lux::ecs::inComponentView<
                    lux::ecs::ClassicMeshBatchComponent>(
                        *registry,
                        entity,
                        lux::ecs::ComponentList<
                            lux::ecs::ResolvedTransform3DComponent>{},
                        lux::ecs::ComponentList<>{}))
            {
                retire(entity);
                return;
            }
            dirty.insert(entity);
            topology_dirty = topology_dirty || topology;
        }

        #include <lux/engine/runtime/render/scene/detail/ClassicMeshBatchRenderPreparation.inl>
        void submit(
            lux::ecs::Entity entity,
            Entry& entry,
            const lux::render::RenderUploadClient& upload)
        {
            auto& pending = *entry.pending;
            if (!callbacks->operations.valid())
            {
                fail(entry, ESceneContentRenderFailure::FEATURE_UNAVAILABLE);
                return;
            }
            pending.revision = nextRevision();
            pending.payload.revision = pending.revision;
            pending.payload.scene_id = callbacks->scene;
            const auto bytes = pending.wire->size() * sizeof(
                lux::render::RenderClusterWireInstance);
            auto submitted = upload.trySubmit<
                lux::render::RenderClusterUploadedReply>(
                [wire = pending.wire,
                 payload = pending.payload,
                 operation = callbacks->operations.id<
                     lux::render::RenderClusterUploadOp>()](
                    lux::render::RenderUploadClient::Builder& builder)
                    mutable
                {
                    payload.instances = builder.pushSharedBytes(
                        std::static_pointer_cast<const void>(wire),
                        reinterpret_cast<const std::byte*>(wire->data()),
                        static_cast<std::uint32_t>(wire->size() * sizeof(
                            lux::render::RenderClusterWireInstance)),
                        lux::render::attachment_types::OwnedBytes,
                        wire->size() * sizeof(
                            lux::render::RenderClusterWireInstance));
                    builder.pushPreparedResource(operation, payload);
                },
                lux::render::UploadPayloadAccounting{
                    .shared_bytes = bytes});
            if (!submitted)
            {
                fail(
                    entry,
                    lux::ecs::isRenderUploadBackpressure(submitted.error())
                        ? ESceneContentRenderFailure::UPLOAD_BACKPRESSURE
                        : ESceneContentRenderFailure::UPLOAD_REJECTED);
                return;
            }

            pending.upload_submitted = true;
            entry.state = ESceneContentRenderState::UPLOADING;
            const auto owner_generation = pending.owner_generation;
            const auto revision = pending.revision;
            const auto id = pending.id;
            auto request = std::move(*submitted);
            const auto started = uploads.start(
                revision,
                UploadContext{entity, owner_generation, revision, id},
                [request = std::move(request)]() mutable
                {
                    return std::move(request);
                });
            if (started != lux::ecs::ETrackedRequestStart::STARTED)
            {
                sendRemove(id, nextRevision(), true);
                fail(entry, ESceneContentRenderFailure::UPLOAD_REJECTED);
            }
        }

        void uploadFinished(
            lux::ecs::Entity entity,
            std::uint64_t owner_generation,
            std::uint64_t revision,
            lux::render::RenderClusterWireId id,
            const lux::render::RenderClusterUploadedReply& reply,
            bool transport_failed) noexcept
        {
            const auto found = entries.find(entity);
            const bool owner_matches = found != entries.end() &&
                found->second.owner_generation == owner_generation;
            const bool request_matches = owner_matches &&
                found->second.pending &&
                found->second.pending->revision == revision &&
                reply.id == id;
            const auto disposition = detail::classifyContentUploadReply(
                owner_matches,
                request_matches,
                transport_failed,
                reply.status);
            if (disposition == detail::
                    EContentUploadReplyDisposition::COMPENSATE_REMOVE)
            {
                ++metrics.stale_success_replies;
                sendRemove(id, nextRevision(), true);
                return;
            }
            if (!request_matches)
                return;
            auto& entry = found->second;
            if (disposition == detail::
                    EContentUploadReplyDisposition::FAIL_DOMAIN)
            {
                fail(entry, ESceneContentRenderFailure::UPLOAD_REJECTED);
                return;
            }

            Active active;
            active.revision = revision;
            active.item_count = reply.instance_count;
            active.id = id;
            active.blob = std::move(entry.pending->blob);
            active.assets.reserve(entry.pending->assets.size());
            for (auto& need : entry.pending->assets)
                active.assets.push_back(std::move(need.reference));
            const auto launched_generation =
                entry.pending->desired_generation;
            entry.active.emplace(std::move(active));
            entry.pending.reset();
            entry.failure = ESceneContentRenderFailure::NONE;
            entry.state = ESceneContentRenderState::ACTIVE;
            if (entry.desired_generation != launched_generation)
                dirty.insert(entity);
        }

        void drive(lux::ecs::RenderSubsystemContext& context)
        {
            uploads.drain(
                [this](auto completion)
                {
                    if (completion.abandoned)
                    {
                        if (!completion.dispatch_failed &&
                            completion.reply.status == 0u)
                        {
                            ++metrics.stale_success_replies;
                            sendRemove(
                                completion.context.id,
                                nextRevision(),
                                true);
                        }
                        return;
                    }
                    uploadFinished(
                        completion.context.entity,
                        completion.context.owner_generation,
                        completion.context.revision,
                        completion.context.id,
                        completion.reply,
                        completion.dispatch_failed);
                });
            scene_origin = context.render().sceneOriginTile3D();
            if (topology_dirty && registry)
            {
                rebuildTopologyIndex();
                const auto view = registry->view<
                    const lux::ecs::ClassicMeshBatchComponent,
                    const lux::ecs::ResolvedTransform3DComponent>();
                for (const auto entity : view)
                    dirty.insert(entity);
                topology_dirty = false;
            }

            auto pending_dirty = std::move(dirty);
            dirty.clear();
            for (const auto entity : pending_dirty)
            {
                if (!registry->valid(entity) || !registry->all_of<
                        lux::ecs::ClassicMeshBatchComponent,
                        lux::ecs::ResolvedTransform3DComponent>(entity))
                {
                    retire(entity);
                    continue;
                }
                auto [it, inserted] = entries.try_emplace(entity);
                auto& entry = it->second;
                if (inserted)
                    entry.owner_generation = owner_generations.next();
                ++entry.desired_generation;
                if (entry.desired_generation == 0u)
                    ++entry.desired_generation;
                if (entry.preparation || entry.pending)
                {
                    ++metrics.coalesced_patches;
                    continue;
                }
                begin(entity, entry);
            }

            for (auto& [entity, entry] : entries)
            {
                if (entry.preparation &&
                    !entry.preparation->in_flight &&
                    entry.state ==
                        ESceneContentRenderState::WAITING_CONTENT)
                {
                    launchPreparation(entity, entry);
                }
            }

            for (auto& [entity, entry] : entries)
            {
                if (!entry.pending || entry.pending->upload_submitted ||
                    entry.state != ESceneContentRenderState::WAITING_CONTENT)
                {
                    continue;
                }
                if (!buildRows(entity, entry))
                {
                    fail(entry, ESceneContentRenderFailure::INVALID_TRANSFORM);
                    continue;
                }
                if (entry.pending && entry.pending->next_row ==
                        entry.pending->decoded->instances.size())
                {
                    submit(entity, entry, context.render().upload());
                }
            }
            uploads.drain(
                [this](auto completion)
                {
                    if (completion.abandoned)
                    {
                        if (!completion.dispatch_failed &&
                            completion.reply.status == 0u)
                        {
                            ++metrics.stale_success_replies;
                            sendRemove(
                                completion.context.id,
                                nextRevision(),
                                true);
                        }
                        return;
                    }
                    uploadFinished(
                        completion.context.entity,
                        completion.context.owner_generation,
                        completion.context.revision,
                        completion.context.id,
                        completion.reply,
                        completion.dispatch_failed);
                });
        }

        SceneContentRenderEntrySnapshot status(
            lux::ecs::Entity entity) const noexcept
        {
            const auto found = entries.find(entity);
            if (found == entries.end())
                return {};
            const auto& entry = found->second;
            return {
                entry.state,
                entry.failure,
                entry.desired_generation,
                entry.active ? entry.active->revision : 0u,
                entry.active ? entry.active->item_count : 0u};
        }

        SceneContentRenderSubsystemSnapshot snapshot() const noexcept
        {
            auto result = metrics;
            result.tracked_entities = entries.size();
            for (const auto& [_, entry] : entries)
            {
                result.active_entities += entry.active.has_value();
                result.failed_entities +=
                    entry.state == ESceneContentRenderState::FAILED;
                result.pending_uploads += entry.pending.has_value();
                result.pending_preparations +=
                    entry.preparation.has_value();
            }
            return result;
        }

        entity_scene::ContentBlobClient blobs;
        lux::ecs::ResidencyCallbacks residency;
        lux::asset::AssetManager* assets{nullptr};
        ClassicMeshPrepareClient preparation_client;
        lux::exec::AsyncScope* async_scope{nullptr};
        std::shared_ptr<CallbackControl> callbacks;
        lux::ecs::Registry* registry{nullptr};
        lux::ecs::EcsCommandWriter commands;
        lux::math::GridCoord3i64 scene_origin{};
        std::unordered_map<lux::ecs::Entity, Entry> entries;
        std::unordered_set<lux::ecs::Entity> dirty;
        std::unordered_map<
            uuids::uuid,
            std::vector<lux::ecs::Entity>> children_by_parent;
        lux::ecs::TrackedRenderRequest<
            std::uint64_t,
            lux::render::RenderClusterUploadedReply,
            UploadContext> uploads;
        detail::ContentRenderRevisionSequence revisions;
        detail::ContentRenderOwnerSequence owner_generations;
        bool topology_dirty{false};
        bool closed{false};
        SceneContentRenderSubsystemSnapshot metrics;
        lux::ecs::ComponentSetChangeObserver<
            lux::ecs::ClassicMeshBatchComponent,
            lux::ecs::ComponentList<
                lux::ecs::ResolvedTransform3DComponent>,
            lux::ecs::ComponentList<>> changes;
        lux::ecs::ComponentSetLeaveObserver<
            lux::ecs::ClassicMeshBatchComponent,
            lux::ecs::ComponentList<
                lux::ecs::ResolvedTransform3DComponent>,
            lux::ecs::ComponentList<>> leaves;
        entt::scoped_connection lod_constructed;
        entt::scoped_connection lod_updated;
        entt::scoped_connection lod_destroyed;
        entt::scoped_connection parent_constructed;
        entt::scoped_connection parent_updated;
        entt::scoped_connection parent_destroyed;
        entt::scoped_connection persistent_constructed;
        entt::scoped_connection persistent_destroyed;
        entt::scoped_connection transform_updated;
    };

    ClassicMeshBatchRenderSubsystem::ClassicMeshBatchRenderSubsystem(
        entity_scene::ContentBlobClient blobs,
        lux::ecs::ResidencyCallbacks residency,
        lux::asset::AssetManager& assets,
        ClassicMeshPrepareClient preparation,
        SceneAsyncContext& async) noexcept
        : impl_(std::make_unique<Impl>(
              std::move(blobs),
              std::move(residency),
              assets,
              std::move(preparation),
              async))
    {
        impl_->callbacks->owner = this;
    }

    ClassicMeshBatchRenderSubsystem::~ClassicMeshBatchRenderSubsystem()
    {
        impl_->callbacks->owner = nullptr;
        impl_->detach();
    }

    void ClassicMeshBatchRenderSubsystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        impl_->attach(setup.registry(), setup.commands());
    }

    void ClassicMeshBatchRenderSubsystem::onRemoved(
        const lux::ecs::SystemRemovalContext&)
    {
        impl_->detach();
    }

    std::span<const lux::ecs::RenderSubsystemType>
    ClassicMeshBatchRenderSubsystem::runsAfter() const noexcept
    {
        static constexpr std::array dependencies{
            lux::ecs::renderSubsystemType<lux::ecs::ResidencySubsystem>()};
        return dependencies;
    }

    std::span<const std::string_view>
    ClassicMeshBatchRenderSubsystem::renderFeatures() const noexcept
    {
        static constexpr std::array<std::string_view, 1u> features{
            "RenderCluster"};
        return features;
    }

    void ClassicMeshBatchRenderSubsystem::prepare(
        lux::ecs::RenderSubsystemContext& context) noexcept
    {
        impl_->prepare(context);
    }

    void ClassicMeshBatchRenderSubsystem::update(
        lux::ecs::RenderSubsystemContext& context)
    {
        if (!impl_->closed)
            impl_->drive(context);
    }

    void ClassicMeshBatchRenderSubsystem::close(
        lux::ecs::RenderSubsystemContext& context) noexcept
    {
        if (impl_->closed)
            return;
        impl_->prepare(context);
        impl_->closed = true;
        impl_->detach();
        impl_->callbacks->owner = nullptr;
        std::vector<lux::ecs::Entity> entities;
        entities.reserve(impl_->entries.size());
        for (const auto& [entity, _] : impl_->entries)
            entities.push_back(entity);
        for (const auto entity : entities)
            impl_->retire(entity);
    }

    SceneContentRenderEntrySnapshot ClassicMeshBatchRenderSubsystem::status(
        lux::ecs::Entity entity) const noexcept
    {
        return impl_->status(entity);
    }

    SceneContentRenderSubsystemSnapshot
    ClassicMeshBatchRenderSubsystem::snapshot() const noexcept
    {
        return impl_->snapshot();
    }

    void ClassicMeshBatchRenderSubsystem::applyObservedChange(
        lux::ecs::Entity entity,
        bool topology) noexcept
    {
        impl_->observed(entity, topology);
    }
} // namespace lux::runtime
