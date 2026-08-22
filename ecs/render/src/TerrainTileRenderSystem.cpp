#include <lux/engine/ecs/render/systems/3d/TerrainTileRenderSystem.hpp>
#include <lux/engine/ecs/render/systems/RenderSystem.hpp>
#include <lux/engine/ecs/render/SceneGeometryPreparation.hpp>
#include <lux/engine/ecs/render/detail/SceneContentRenderContracts.hpp>

#include <lux/engine/ecs/components/PersistentEntityIdComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include <lux/engine/ecs/render/TrackedRenderRequest.hpp>
#include <lux/engine/ecs/render/RenderViewUtil.hpp>
#include <lux/engine/ecs/terrain/components/TerrainLodNodeComponent.hpp>
#include <lux/engine/ecs/terrain/components/TerrainTileComponent.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/genops/TerrainOperation.ops.hpp>
#include <lux/engine/ecs/terrain/TerrainTileCodec.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::ecs
{
    using lux::ecs::ESceneGeometryPrepareError;
    using lux::ecs::PrepareTerrainTile;
    using lux::ecs::SceneGeometryPrepareFailure;

    namespace
    {
        [[nodiscard]] lux::render::TerrainWireId terrainUuidWireId(
            const lux::ecs::PersistentEntityId& id) noexcept
        {
            lux::render::TerrainWireId result{};
            if (id.empty())
                return result;
            const auto bytes = id.value().as_bytes();
            static_assert(sizeof(result.bytes) == 16u);
            std::memcpy(result.bytes, bytes.data(), sizeof(result.bytes));
            return result;
        }

        [[nodiscard]] lux::render::TerrainWireId terrainEntityWireId(
            lux::ecs::Registry& registry,
            lux::ecs::Entity entity) noexcept
        {
            if (const auto* persistent = registry.try_get<
                    lux::ecs::PersistentEntityIdComponent>(entity))
            {
                return terrainUuidWireId(persistent->id());
            }
            lux::render::TerrainWireId result{};
            const auto raw = static_cast<std::uint64_t>(
                entt::to_integral(entity));
            constexpr std::uint64_t salt = 0x54524e52544c5558ull;
            std::memcpy(result.bytes, &raw, sizeof(raw));
            std::memcpy(result.bytes + sizeof(raw), &salt, sizeof(salt));
            return result;
        }

    } // namespace

    struct TerrainTileObservedCommand final
    {
        using Producer = TerrainTileRenderSystem;

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
            TerrainTileRenderSystem& owner) const noexcept
        {
            owner.applyObservedChange(entity, topology);
        }
    };
    static_assert(std::is_trivially_copyable_v<TerrainTileObservedCommand>);

    struct TerrainTileRenderSystem::Impl final
    {
        struct Pending final
        {
            std::uint64_t desired_generation{0u};
            std::uint64_t owner_generation{0u};
            std::uint64_t revision{0u};
            lux::render::TerrainWireId id{};
            lux::render::UploadTerrainPagePayload payload{};
            lux::ecs::entity_scene::ContentBlobLease blob;
            std::shared_ptr<std::vector<std::byte>> wire;
            bool upload_submitted{false};
        };

        struct Preparation final
        {
            std::uint64_t desired_generation{0u};
            std::uint64_t owner_generation{0u};
            lux::ecs::entity_scene::ContentBlobLease blob;
            bool in_flight{false};
        };

        struct Active final
        {
            std::uint64_t revision{0u};
            lux::render::TerrainWireId id{};
            lux::ecs::entity_scene::ContentBlobLease blob;
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
            TerrainTileRenderSystem* owner{nullptr};
            lux::render::RenderControlSession* control{nullptr};
            lux::render::RenderSceneId scene{};
            lux::render::TerrainOperationIds operations{};
        };

        struct UploadContext final
        {
            lux::ecs::Entity entity{entt::null};
            std::uint64_t owner_generation{0u};
            std::uint64_t revision{0u};
            lux::render::TerrainWireId id{};
        };

        Impl(
            lux::ecs::entity_scene::ContentBlobClient client,
            lux::ecs::TerrainPreparePort preparation_value) noexcept
            : blobs(std::move(client)),
              preparation_client(std::move(preparation_value)),
              callbacks(std::make_shared<CallbackControl>())
        {}

        [[nodiscard]] std::uint64_t nextRevision() noexcept
        {
            return revisions.next();
        }

        void enqueue(lux::ecs::Entity entity, bool topology) noexcept
        {
            (void)commands.push(TerrainTileObservedCommand{entity, topology});
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
                lux::ecs::TerrainLodNodeComponent>()
                .connect<&Impl::onTopology>(*this);
            lod_updated = value.on_update<
                lux::ecs::TerrainLodNodeComponent>()
                .connect<&Impl::onTopology>(*this);
            lod_destroyed = value.on_destroy<
                lux::ecs::TerrainLodNodeComponent>()
                .connect<&Impl::onTopology>(*this);
            parent_constructed = value.on_construct<
                lux::ecs::TerrainLodParentComponent>()
                .connect<&Impl::onTopology>(*this);
            parent_updated = value.on_update<
                lux::ecs::TerrainLodParentComponent>()
                .connect<&Impl::onTopology>(*this);
            parent_destroyed = value.on_destroy<
                lux::ecs::TerrainLodParentComponent>()
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

        void prepare(lux::ecs::SceneRenderBinding& render) noexcept
        {
            scene_origin = render.sceneOriginTile3D();
            callbacks->control = &render.control();
            callbacks->scene = render.scene();
            callbacks->operations = render.features().ops<
                lux::render::TerrainOperationIds>("Terrain");
        }

        void sendRemove(
            lux::render::TerrainWireId id,
            std::uint64_t revision,
            bool compensation = false) noexcept
        {
            if (!id.valid() || !callbacks->control ||
                !callbacks->operations.valid() || callbacks->scene.isNull())
            {
                return;
            }
            (void)lux::render::TerrainControlClient{
                *callbacks->control, callbacks->operations}.remove(
                    lux::render::RemoveTerrainPagePayload{
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
            lux::render::TerrainWireId id{};
            if (entry.pending)
                id = entry.pending->id;
            else if (entry.active)
                id = entry.active->id;
            else if (registry)
                id = terrainEntityWireId(*registry, entity);
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
                    lux::ecs::TerrainTileComponent>(
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

        void fail(Entry& entry, ESceneContentRenderFailure failure) noexcept
        {
            entry.preparation.reset();
            entry.pending.reset();
            entry.failure = failure;
            entry.state = ESceneContentRenderState::FAILED;
        }

        [[nodiscard]] bool fillTopology(
            lux::ecs::Entity entity,
            lux::render::UploadTerrainPagePayload& payload) const noexcept
        {
            if (const auto* lod = registry->try_get<
                    lux::ecs::TerrainLodNodeComponent>(entity))
            {
                if (!detail::validVisualLodContract(
                        lod->geometric_error,
                        lod->enter_error_pixels,
                        lod->exit_error_pixels) ||
                    lod->exit_error_pixels <= 0.0f || lod->level > 4u)
                {
                    return false;
                }
                payload.hierarchy_level = lod->level;
                payload.geometric_error = lod->geometric_error;
                payload.hlod_enter_error_pixels = lod->enter_error_pixels;
                payload.hlod_exit_error_pixels = lod->exit_error_pixels;
            }
            if (const auto* parent = registry->try_get<
                    lux::ecs::TerrainLodParentComponent>(entity))
            {
                if (!parent->parent.valid() || !registry->all_of<
                        lux::ecs::PersistentEntityIdComponent>(entity))
                    return false;
                payload.parent = terrainUuidWireId(parent->parent.id);
            }

            const auto* own = registry->try_get<
                lux::ecs::PersistentEntityIdComponent>(entity);
            if (!own)
                return true;
            const auto children = children_by_parent.find(own->id().value());
            if (children != children_by_parent.end())
            {
                for (const auto child : children->second)
                {
                    if (payload.child_count >= 16u)
                        return false;
                    payload.children[payload.child_count++] =
                        terrainUuidWireId(
                            registry->get<const lux::ecs::
                                PersistentEntityIdComponent>(child).id());
                }
            }
            return detail::validTerrainLodContract(
                payload.hierarchy_level,
                payload.child_count,
                payload.geometric_error,
                payload.hlod_enter_error_pixels,
                payload.hlod_exit_error_pixels);
        }

        void rebuildTopologyIndex()
        {
            children_by_parent.clear();
            const auto children = registry->view<
                const lux::ecs::TerrainTileComponent,
                const lux::ecs::TerrainLodParentComponent,
                const lux::ecs::PersistentEntityIdComponent>();
            children_by_parent.reserve(children.size_hint());
            for (const auto child : children)
            {
                const auto& parent = children.get<const
                    lux::ecs::TerrainLodParentComponent>(child).parent;
                if (parent.valid())
                    children_by_parent[parent.id.value()].push_back(child);
            }
        }

        void begin(lux::ecs::Entity entity, Entry& entry)
        {
            const auto& component = registry->get<
                lux::ecs::TerrainTileComponent>(entity);
            const auto& transform = registry->get<
                lux::ecs::ResolvedTransform3DComponent>(entity);
            if (!component.content.valid() ||
                component.content.type.name() !=
                    lux::terrain::kTerrainTileContentTypeName ||
                component.content.schema_version !=
                    lux::terrain::kTerrainTileSchemaVersion ||
                !transform.linear.allFinite() ||
                !lux::math::isFinite(transform.position))
            {
                fail(entry, ESceneContentRenderFailure::INVALID_COMPONENT);
                return;
            }
            constexpr float tolerance = 1.0e-4f;
            const auto diagonal = transform.linear.diagonal();
            Eigen::Matrix3f off_diagonal = transform.linear;
            off_diagonal.diagonal().setZero();
            if (off_diagonal.cwiseAbs().maxCoeff() > tolerance ||
                diagonal.minCoeff() <= 0.0f ||
                std::abs(diagonal.x() - diagonal.z()) > tolerance)
            {
                fail(entry, ESceneContentRenderFailure::INVALID_TRANSFORM);
                return;
            }

            auto lease = blobs.resolve(component.content);
            if (!lease)
            {
                fail(entry, ESceneContentRenderFailure::CONTENT_UNAVAILABLE);
                return;
            }
            entry.preparation.emplace(Preparation{
                entry.desired_generation,
                entry.owner_generation,
                std::move(*lease),
                false});
            entry.failure = ESceneContentRenderFailure::NONE;
            entry.state = ESceneContentRenderState::WAITING_CONTENT;
            launchPreparation(entity, entry);
        }

        [[nodiscard]] ESceneContentRenderFailure preparationFailure(
            const lux::async::OperationFailure<SceneGeometryPrepareFailure>&
                value) const noexcept
        {
            if (value.isRuntime())
                return ESceneContentRenderFailure::FEATURE_UNAVAILABLE;
            switch (value.domainError().code)
            {
            case ESceneGeometryPrepareError::CONTENT_MISMATCH:
                return ESceneContentRenderFailure::CONTENT_MISMATCH;
            case ESceneGeometryPrepareError::UNSUPPORTED_CONTENT:
            case ESceneGeometryPrepareError::INVALID_REQUEST:
                return ESceneContentRenderFailure::INVALID_COMPONENT;
            case ESceneGeometryPrepareError::DECODE_FAILED:
                return ESceneContentRenderFailure::DECODE_FAILED;
            case ESceneGeometryPrepareError::SERVICE_CLOSED:
                return ESceneContentRenderFailure::FEATURE_UNAVAILABLE;
            }
            return ESceneContentRenderFailure::DECODE_FAILED;
        }

        void launchPreparation(
            lux::ecs::Entity entity,
            Entry& entry) noexcept
        {
            if (!entry.preparation || entry.preparation->in_flight)
                return;
            if (!preparation_client)
            {
                fail(entry, ESceneContentRenderFailure::FEATURE_UNAVAILABLE);
                return;
            }
            auto& preparing = *entry.preparation;
            const auto owner_generation = preparing.owner_generation;
            const auto desired_generation = preparing.desired_generation;
            preparing.in_flight = true;
            entry.state = ESceneContentRenderState::WAITING_BACKGROUND;
            struct Completion final
            {
                std::weak_ptr<CallbackControl> callbacks;
                lux::ecs::Entity entity{entt::null};
                std::uint64_t owner_generation{0u};
                std::uint64_t desired_generation{0u};
            };
            auto* completion = new Completion{
                callbacks,
                entity,
                owner_generation,
                desired_generation};
            (void)preparation_client.submit(
                PrepareTerrainTile{
                    preparing.blob.bytes(),
                    preparing.blob.reference(),
                    desired_generation},
                completion,
                +[](void* opaque,
                    lux::async::OperationOutcome<PrepareTerrainTile>&&
                        outcome) noexcept
                {
                    std::unique_ptr<Completion> state{
                        static_cast<Completion*>(opaque)};
                    const auto locked = state->callbacks.lock();
                    if (locked && locked->owner)
                    {
                        locked->owner->impl_->acceptPreparation(
                            state->entity,
                            state->owner_generation,
                            state->desired_generation,
                            std::move(outcome));
                    }
                });
        }

        void acceptPreparation(
            lux::ecs::Entity entity,
            std::uint64_t owner_generation,
            std::uint64_t desired_generation,
            lux::async::OperationOutcome<PrepareTerrainTile> outcome) noexcept
        {
            const auto found = entries.find(entity);
            const bool owner_matches = found != entries.end() &&
                found->second.owner_generation == owner_generation;
            const bool request_matches = owner_matches &&
                found->second.preparation &&
                found->second.preparation->desired_generation ==
                    desired_generation;
            const bool desired_matches = request_matches &&
                found->second.desired_generation == desired_generation;
            const auto disposition = detail::classifyContentPreparation(
                owner_matches, request_matches, desired_matches);
            if (disposition == detail::
                    EContentPreparationDisposition::DISCARD_STALE)
            {
                ++metrics.stale_preparation_completions;
                return;
            }
            auto& entry = found->second;
            if (disposition == detail::
                    EContentPreparationDisposition::RETRY_LATEST)
            {
                entry.preparation.reset();
                dirty.insert(entity);
                ++metrics.stale_preparation_completions;
                return;
            }
            if (!outcome)
            {
                if (outcome.error().isRuntime() &&
                    (outcome.error().runtimeError() ==
                         lux::async::ESubmitError::QUEUE_FULL ||
                     outcome.error().runtimeError() ==
                         lux::async::ESubmitError::
                             BYTE_BUDGET_EXHAUSTED))
                {
                    entry.preparation->in_flight = false;
                    entry.state = ESceneContentRenderState::WAITING_CONTENT;
                    ++metrics.preparation_backpressure;
                    return;
                }
                fail(entry, preparationFailure(outcome.error()));
                return;
            }
            auto prepared = std::move(*outcome);
            if (prepared.request_generation != desired_generation ||
                !prepared.wire)
            {
                fail(entry, ESceneContentRenderFailure::DECODE_FAILED);
                return;
            }
            if (!registry || !registry->valid(entity) ||
                !registry->all_of<
                    lux::ecs::TerrainTileComponent,
                    lux::ecs::ResolvedTransform3DComponent>(entity))
            {
                retire(entity);
                return;
            }
            const auto& transform = registry->get<
                lux::ecs::ResolvedTransform3DComponent>(entity);
            constexpr float tolerance = 1.0e-4f;
            const auto diagonal = transform.linear.diagonal();
            Eigen::Matrix3f off_diagonal = transform.linear;
            off_diagonal.diagonal().setZero();
            if (!transform.linear.allFinite() ||
                !lux::math::isFinite(transform.position) ||
                off_diagonal.cwiseAbs().maxCoeff() > tolerance ||
                diagonal.minCoeff() <= 0.0f ||
                std::abs(diagonal.x() - diagonal.z()) > tolerance)
            {
                fail(entry, ESceneContentRenderFailure::INVALID_TRANSFORM);
                return;
            }
            Pending pending;
            pending.desired_generation = desired_generation;
            pending.owner_generation = owner_generation;
            pending.id = terrainEntityWireId(*registry, entity);
            pending.payload.scene_id = callbacks->scene;
            pending.payload.id = pending.id;
            const auto origin = lux::ecs::makeRenderLargePosition(
                transform.position, scene_origin);
            if (!origin)
            {
                fail(entry, ESceneContentRenderFailure::INVALID_TRANSFORM);
                return;
            }
            pending.payload.origin = *origin;
            pending.payload.height_min =
                prepared.height_min * diagonal.y();
            pending.payload.height_max =
                prepared.height_max * diagonal.y();
            pending.payload.sample_spacing =
                prepared.sample_spacing * diagonal.x();
            pending.payload.weight_layer_count =
                prepared.weight_layer_count;
            pending.payload.transition_milliseconds = 350u;
            std::uint32_t seed = 0u;
            std::memcpy(&seed, pending.id.bytes, sizeof(seed));
            pending.payload.transition_seed = seed == 0u ? 1u : seed;
            if (!fillTopology(entity, pending.payload))
            {
                fail(entry, ESceneContentRenderFailure::INVALID_LOD_TOPOLOGY);
                return;
            }
            pending.blob = std::move(entry.preparation->blob);
            pending.wire = std::move(prepared.wire);
            entry.preparation.reset();
            entry.pending.emplace(std::move(pending));
            entry.failure = ESceneContentRenderFailure::NONE;
            entry.state = ESceneContentRenderState::WAITING_CONTENT;
        }

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
            auto submitted = upload.trySubmit<
                lux::render::TerrainPageUploadedReply>(
                [wire = pending.wire,
                 payload = pending.payload,
                 operation = callbacks->operations.id<
                     lux::render::TerrainPageUploadOp>()](
                    lux::render::RenderUploadClient::Builder& builder)
                    mutable
                {
                    payload.page_data = builder.pushSharedBytes(
                        std::static_pointer_cast<const void>(wire),
                        wire->data(),
                        static_cast<std::uint32_t>(wire->size()),
                        lux::render::attachment_types::OwnedBytes,
                        wire->size());
                    builder.pushPreparedResource(operation, payload);
                },
                lux::render::UploadPayloadAccounting{
                    .shared_bytes = pending.wire->size()});
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
            lux::render::TerrainWireId id,
            const lux::render::TerrainPageUploadedReply& reply,
            bool transport_failed) noexcept
        {
            const auto found = entries.find(entity);
            const bool owner_matches = found != entries.end() &&
                found->second.owner_generation == owner_generation;
            const bool request_matches = owner_matches &&
                found->second.pending &&
                found->second.pending->revision == revision &&
                reply.id == id && reply.revision == revision;
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
            active.id = id;
            active.blob = std::move(entry.pending->blob);
            const auto launched_generation =
                entry.pending->desired_generation;
            entry.active.emplace(std::move(active));
            entry.pending.reset();
            entry.failure = ESceneContentRenderFailure::NONE;
            entry.state = ESceneContentRenderState::ACTIVE;
            if (entry.desired_generation != launched_generation)
                dirty.insert(entity);
        }

        void drive(lux::ecs::SceneRenderBinding& render)
        {
            scene_origin = render.sceneOriginTile3D();
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
            if (topology_dirty && registry)
            {
                rebuildTopologyIndex();
                const auto view = registry->view<
                    const lux::ecs::TerrainTileComponent,
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
                        lux::ecs::TerrainTileComponent,
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
                if (entry.pending && !entry.pending->upload_submitted &&
                    entry.state == ESceneContentRenderState::WAITING_CONTENT)
                {
                    submit(entity, entry, render.upload());
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
                entry.active ? 1u : 0u};
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

        lux::ecs::entity_scene::ContentBlobClient blobs;
        lux::ecs::TerrainPreparePort preparation_client;
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
            lux::render::TerrainPageUploadedReply,
            UploadContext> uploads;
        detail::ContentRenderRevisionSequence revisions;
        detail::ContentRenderOwnerSequence owner_generations;
        bool topology_dirty{false};
        bool closed{false};
        SceneContentRenderSubsystemSnapshot metrics;
        lux::ecs::ComponentSetChangeObserver<
            lux::ecs::TerrainTileComponent,
            lux::ecs::ComponentList<
                lux::ecs::ResolvedTransform3DComponent>,
            lux::ecs::ComponentList<>> changes;
        lux::ecs::ComponentSetLeaveObserver<
            lux::ecs::TerrainTileComponent,
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

    TerrainTileRenderSystem::TerrainTileRenderSystem(
        lux::ecs::SceneRenderBinding& render,
        lux::ecs::entity_scene::ContentBlobClient blobs,
        lux::ecs::TerrainPreparePort preparation) noexcept
        : render_(&render),
          impl_(std::make_unique<Impl>(
              std::move(blobs), std::move(preparation)))
    {
        impl_->callbacks->owner = this;
    }

    TerrainTileRenderSystem::~TerrainTileRenderSystem()
    {
        impl_->callbacks->owner = nullptr;
        impl_->detach();
    }

    void TerrainTileRenderSystem::onAdded(
        const lux::ecs::SystemSetupContext& setup)
    {
        impl_->attach(setup.registry(), setup.commands());
    }

    void TerrainTileRenderSystem::onRemoved(
        const lux::ecs::SystemRemovalContext&)
    {
        impl_->detach();
    }

    std::span<const std::string_view>
    TerrainTileRenderSystem::requiredRenderFeatures() noexcept
    {
        static constexpr std::array<std::string_view, 1u> features{
            "Terrain"};
        return features;
    }

    void TerrainTileRenderSystem::update(
        const lux::ecs::SystemUpdateContext&)
    {
        if (!impl_->closed)
        {
            impl_->prepare(*render_);
            impl_->drive(*render_);
        }
    }

    void TerrainTileRenderSystem::requestClose() noexcept
    {
        if (impl_->closed)
            return;
        impl_->prepare(*render_);
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

    std::span<const lux::ecs::ISystem::Type>
    TerrainTileRenderSystem::runsAfter() const noexcept
    {
        static constexpr std::array<Type, 1u> dependencies{
            lux::ecs::systemType<lux::ecs::RenderSystem>()};
        return dependencies;
    }

    SceneContentRenderEntrySnapshot TerrainTileRenderSystem::status(
        lux::ecs::Entity entity) const noexcept
    {
        return impl_->status(entity);
    }

    SceneContentRenderSubsystemSnapshot
    TerrainTileRenderSystem::snapshot() const noexcept
    {
        return impl_->snapshot();
    }

    void TerrainTileRenderSystem::applyObservedChange(
        lux::ecs::Entity entity,
        bool topology) noexcept
    {
        impl_->observed(entity, topology);
    }
} // namespace lux::ecs
