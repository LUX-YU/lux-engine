#include <lux/engine/scene/Builtin3DRenderStages.hpp>

#include <lux/engine/function/render/client/features/light/LightOperation.hpp>
#include <lux/engine/function/render/client/features/meshstack/MeshStackOperation.hpp>
#include <lux/engine/scene/RenderSystem.hpp>
#include <lux/engine/scene/ResolvedMeshResources.hpp>
#include <lux/engine/simulation/ecs/ComponentChangeSet.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <Eigen/Core>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace lux::scene
{
    namespace
    {
        using simulation::ecs::ComponentList;
        using simulation::ecs::Entity;
        using simulation::ecs::Registry;

        [[nodiscard]] bool encodePosition(
            const Eigen::Vector3d& absolute_position,
            double page_size,
            const std::array<std::int64_t, 3>& origin,
            render::RenderLargePosition3D& output
        ) noexcept
        {
            if (!absolute_position.allFinite())
            {
                return false;
            }
            for (std::size_t axis = 0U; axis < 3U; ++axis)
            {
                const long double absolute = static_cast<long double>(absolute_position[axis]);
                const long double size = static_cast<long double>(page_size);
                const long double page_value = std::floor(absolute / size);
                const bool is_page_out_of_range = page_value < std::numeric_limits<std::int64_t>::min() ||
                    page_value > std::numeric_limits<std::int64_t>::max();
                if (is_page_out_of_range)
                {
                    return false;
                }
                const long double page_delta = page_value - static_cast<long double>(origin[axis]);
                const bool is_delta_out_of_range = page_delta < std::numeric_limits<std::int32_t>::min() ||
                    page_delta > std::numeric_limits<std::int32_t>::max();
                if (is_delta_out_of_range)
                {
                    return false;
                }
                const long double local = absolute - page_value * size;
                const float narrowed = static_cast<float>(local);
                if (!std::isfinite(narrowed))
                {
                    return false;
                }
                output.page_delta[axis] = static_cast<std::int32_t>(page_delta);
                output.local[axis] = narrowed;
            }
            return true;
        }

        [[nodiscard]] bool encodeTransform(
            const simulation::ecs::WorldTransform3D& world,
            double page_size,
            const std::array<std::int64_t, 3>& origin,
            render::RenderSpatialTransform3D& output
        ) noexcept
        {
            render::RenderLargePosition3D position{};
            if (!encodePosition(world.value.translation(), page_size, origin, position) || !world.value.linear().allFinite())
            {
                return false;
            }
            for (std::size_t column = 0U; column < 3U; ++column)
            {
                for (std::size_t row = 0U; row < 3U; ++row)
                {
                    const float value = static_cast<float>(world.value.linear()(row, column));
                    if (!std::isfinite(value))
                    {
                        return false;
                    }
                    output.basis_local[column * 4U + row] = value;
                }
                output.basis_local[column * 4U + 3U] = position.local[column];
                output.page_delta[column] = position.page_delta[column];
            }
            return true;
        }

        [[nodiscard]] bool sameTransform(
            const render::RenderSpatialTransform3D& left,
            const render::RenderSpatialTransform3D& right
        ) noexcept
        {
            return std::equal(std::begin(left.basis_local), std::end(left.basis_local), std::begin(right.basis_local)) &&
                std::equal(std::begin(left.page_delta), std::end(left.page_delta), std::begin(right.page_delta)) &&
                left.flags == right.flags;
        }

        struct MeshRenderState final
        {
            const void* owner{};
            render::RenderEntityId entity{};
            asset::AssetId mesh_source{};
            asset::AssetId material_source{};
            render::RMeshHandle mesh{};
            render::RMaterialHandle material{};
            render::RenderSpatialTransform3D transform{};
            std::uint32_t flags{};
            bool published{false};
        };
        static_assert(std::is_nothrow_copy_assignable_v<MeshRenderState>);

        struct MeshDeparture final
        {
            render::RenderEntityId entity{};
            Entity source{simulation::ecs::NullEntity};
            bool was_published{false};
        };

        struct MeshStateUpdate final
        {
            Entity entity{simulation::ecs::NullEntity};
            MeshRenderState state{};
        };

        template <class Departure>
        void coalesceDepartures(std::vector<Departure>& departures) noexcept
        {
            std::ranges::sort(departures, [](const Departure& left, const Departure& right) {
                return static_cast<std::uint64_t>(left.entity) < static_cast<std::uint64_t>(right.entity);
            });
            std::size_t output_count{};
            for (const auto& departure : departures)
            {
                if (output_count != 0U && departures[output_count - 1U].entity == departure.entity)
                {
                    departures[output_count - 1U].was_published =
                        departures[output_count - 1U].was_published || departure.was_published;
                    continue;
                }
                departures[output_count++] = departure;
            }
            departures.resize(output_count);
        }

        using MeshChanges = simulation::ecs::ExtractionChangeSet<
            simulation::ecs::Mesh3D,
            ComponentList<simulation::ecs::WorldTransform3D, ResolvedMeshResources>,
            ComponentList<>>;
        using MeshMembershipLeaves = simulation::ecs::ComponentSetLeaveObserver<
            simulation::ecs::Mesh3D,
            ComponentList<simulation::ecs::WorldTransform3D, ResolvedMeshResources>,
            ComponentList<>>;
        using MeshStateLeaves = simulation::ecs::ComponentSetLeaveObserver<
            MeshRenderState,
            ComponentList<>,
            ComponentList<>>;

        class Mesh3DRenderStage final : public RenderSyncStage
        {
        public:
            explicit Mesh3DRenderStage(Mesh3DRenderStageConfig value) : config_(std::move(value))
            {
                using namespace entt::literals;
                changes_.attach(*config_.registry, "scene.mesh3d.render.changes"_hs, [](auto& storage) {
                    storage.template on_construct<simulation::ecs::Mesh3D>()
                        .template on_update<simulation::ecs::Mesh3D>()
                        .template on_construct<simulation::ecs::WorldTransform3D>()
                        .template on_update<simulation::ecs::WorldTransform3D>()
                        .template on_construct<ResolvedMeshResources>()
                        .template on_update<ResolvedMeshResources>();
                });
                membership_leaves_.attach(*config_.registry, this, &Mesh3DRenderStage::onMembershipLeft);
                state_leaves_.attach(*config_.registry, this, &Mesh3DRenderStage::onStateDestroyed);
            }

            ~Mesh3DRenderStage() noexcept override
            {
                changes_.detach();
                membership_leaves_.detach();
                state_leaves_.detach();
                suppress_state_departure_ = true;
                while (true)
                {
                    auto states = config_.registry->view<MeshRenderState>();
                    const auto found = std::find_if(states.begin(), states.end(), [this](Entity entity) {
                        return config_.registry->get<MeshRenderState>(entity).owner == this;
                    });
                    if (found == states.end())
                    {
                        break;
                    }
                    config_.registry->remove<MeshRenderState>(*found);
                }
            }

            [[nodiscard]] bool hasPendingChanges() const noexcept override
            {
                return force_full_sync_ || allocation_failed_ || !departures_.empty() || !changes_.empty();
            }

            void requestFullSync() noexcept override
            {
                force_full_sync_ = true;
            }

            [[nodiscard]] ERenderSyncPrepareResult prepare(render::RenderProgramBuilder<>& builder) noexcept override
            {
                discardPrepared();
                if (allocation_failed_)
                {
                    return ERenderSyncPrepareResult::FAILED;
                }
                try
                {
                    return prepareImpl(builder);
                }
                catch (const std::bad_alloc&)
                {
                    discardPrepared();
                    return ERenderSyncPrepareResult::FAILED;
                }
            }

            // LUX_RENDER_COMMIT_MESH_BEGIN
            void commitPrepared() noexcept override
            {
                if (!prepared_)
                {
                    return;
                }
                suppress_state_departure_ = true;
                for (const auto entity : state_removals_)
                {
                    if (config_.registry->valid(entity))
                    {
                        const auto* state = config_.registry->try_get<MeshRenderState>(entity);
                        if (state != nullptr && state->owner == this)
                        {
                            config_.registry->remove<MeshRenderState>(entity);
                        }
                    }
                }
                for (auto& update : state_updates_)
                {
                    config_.registry->get<MeshRenderState>(update.entity) = update.state;
                }
                suppress_state_departure_ = false;
                changes_.clear();
                departures_.clear();
                force_full_sync_ = false;
                discardPrepared();
            }
            // LUX_RENDER_COMMIT_MESH_END

            void discardPrepared() noexcept override
            {
                state_updates_.clear();
                state_removals_.clear();
                transforms_.clear();
                prepared_ = false;
            }

        private:
            static void onMembershipLeft(void* user, Entity entity) noexcept
            {
                static_cast<Mesh3DRenderStage*>(user)->recordMembershipDeparture(entity);
            }

            static void onStateDestroyed(void* user, Entity entity) noexcept
            {
                static_cast<Mesh3DRenderStage*>(user)->recordStateDeparture(entity);
            }

            void recordMembershipDeparture(Entity entity) noexcept
            {
                const auto* state = config_.registry->try_get<MeshRenderState>(entity);
                if (state != nullptr && state->owner == this)
                {
                    recordDeparture(MeshDeparture{state->entity, entity, state->published});
                }
            }

            void recordStateDeparture(Entity entity) noexcept
            {
                if (suppress_state_departure_)
                {
                    return;
                }
                const auto* state = config_.registry->try_get<MeshRenderState>(entity);
                if (state != nullptr && state->owner == this)
                {
                    recordDeparture(MeshDeparture{state->entity, entity, state->published});
                }
            }

            void recordDeparture(MeshDeparture departure) noexcept
            {
                try
                {
                    departures_.push_back(departure);
                }
                catch (const std::bad_alloc&)
                {
                    allocation_failed_ = true;
                }
            }

            struct MeshInputs final
            {
                const rdesc::MeshVisualDescription* authored{};
                const simulation::ecs::WorldTransform3D* world{};
                const ResolvedMeshResources* resolved{};
            };

            [[nodiscard]] bool currentlyRenderable(Entity entity, MeshInputs& inputs) const noexcept
            {
                if (!config_.registry->valid(entity))
                {
                    return false;
                }
                const auto* mesh = config_.registry->try_get<simulation::ecs::Mesh3D>(entity);
                inputs.world = config_.registry->try_get<simulation::ecs::WorldTransform3D>(entity);
                inputs.resolved = config_.registry->try_get<ResolvedMeshResources>(entity);
                if (mesh == nullptr || inputs.world == nullptr || inputs.resolved == nullptr)
                {
                    return false;
                }
                inputs.authored = &mesh->value;
                return inputs.resolved->mesh_source == inputs.authored->mesh &&
                    inputs.resolved->material_source == inputs.authored->material && !inputs.resolved->mesh.isNull() &&
                    !inputs.resolved->material.isNull();
            }

            [[nodiscard]] bool membershipRestored(const MeshDeparture& departure) const noexcept
            {
                MeshInputs inputs{};
                return config_.registry->valid(departure.source) &&
                    toRenderEntity(departure.source) == departure.entity &&
                    currentlyRenderable(departure.source, inputs);
            }

            [[nodiscard]] bool prepareEntity(render::RenderProgramBuilder<>& builder, Entity entity)
            {
                MeshInputs inputs{};
                if (!currentlyRenderable(entity, inputs))
                {
                    return true;
                }

                MeshRenderState next{};
                next.owner = this;
                next.entity = toRenderEntity(entity);
                next.mesh_source = inputs.resolved->mesh_source;
                next.material_source = inputs.resolved->material_source;
                next.mesh = inputs.resolved->mesh;
                next.material = inputs.resolved->material;
                next.flags = (inputs.authored->visible ? render::kInstanceFlagVisible : 0U) |
                    (inputs.authored->cast_shadow ? render::kInstanceFlagCastShadow : 0U) |
                    (inputs.authored->receive_shadow ? render::kInstanceFlagReceiveShadow : 0U);
                next.published = true;
                if (!encodeTransform(
                        *inputs.world,
                        config_.coordinate_page_size,
                        config_.scene_origin_page,
                        next.transform))
                {
                    return false;
                }

                auto* state_slot = config_.registry->try_get<MeshRenderState>(entity);
                if (state_slot == nullptr)
                {
                    state_slot = &config_.registry->emplace<MeshRenderState>(
                        entity,
                        MeshRenderState{.owner = this, .entity = next.entity}
                    );
                }
                if (state_slot->owner != this)
                {
                    return false;
                }
                const auto* published = state_slot->published ? state_slot : nullptr;
                const bool requires_upsert = force_full_sync_ || published == nullptr ||
                    published->entity != next.entity || published->mesh_source != next.mesh_source ||
                    published->material_source != next.material_source || published->mesh != next.mesh ||
                    published->material != next.material;
                if (requires_upsert)
                {
                    render::UpsertMeshInstancePayload payload{};
                    payload.scene_id = config_.scene;
                    payload.entity = next.entity;
                    payload.mesh = next.mesh;
                    payload.material = next.material;
                    payload.transform = next.transform;
                    payload.flags = next.flags;
                    builder.push(
                        render::opcode_of_v<render::UpsertMeshInstanceOp>,
                        config_.operations.id<render::UpsertMeshInstanceOp>(),
                        payload
                    );
                }
                else
                {
                    if (published->flags != next.flags)
                    {
                        builder.push(
                            render::opcode_of_v<render::UpdateInstanceFlagsOp>,
                            config_.operations.id<render::UpdateInstanceFlagsOp>(),
                            render::UpdateInstanceFlagsPayload{config_.scene, next.entity, next.flags}
                        );
                    }
                    if (!sameTransform(published->transform, next.transform))
                    {
                        transforms_.push_back(render::TransformWriteEntry{config_.scene, next.entity, next.transform});
                    }
                }

                const bool state_changed = published == nullptr || published->entity != next.entity ||
                    published->mesh_source != next.mesh_source || published->material_source != next.material_source ||
                    published->mesh != next.mesh || published->material != next.material ||
                    published->flags != next.flags || !sameTransform(published->transform, next.transform);
                if (state_changed)
                {
                    state_updates_.push_back(MeshStateUpdate{entity, next});
                }
                return true;
            }

            [[nodiscard]] ERenderSyncPrepareResult prepareImpl(render::RenderProgramBuilder<>& builder)
            {
                const std::size_t initial_commands = builder.commandCount();
                coalesceDepartures(departures_);
                for (const auto& departure : departures_)
                {
                    if (membershipRestored(departure))
                    {
                        continue;
                    }
                    if (departure.was_published)
                    {
                        builder.push(
                            render::opcode_of_v<render::RemoveMeshInstanceOp>,
                            config_.operations.id<render::RemoveMeshInstanceOp>(),
                            render::RemoveMeshInstancePayload{config_.scene, departure.entity}
                        );
                    }
                    state_removals_.push_back(departure.source);
                }

                if (force_full_sync_)
                {
                    auto current = componentView<simulation::ecs::Mesh3D>(
                        *config_.registry,
                        ComponentList<simulation::ecs::WorldTransform3D, ResolvedMeshResources>{},
                        ComponentList<>{}
                    );
                    for (const auto entity : current)
                    {
                        if (!prepareEntity(builder, entity))
                        {
                            return ERenderSyncPrepareResult::FAILED;
                        }
                    }
                }
                else
                {
                    for (const auto entity : changes_.view())
                    {
                        if (!prepareEntity(builder, entity))
                        {
                            return ERenderSyncPrepareResult::FAILED;
                        }
                    }
                }

                if (!transforms_.empty())
                {
                    auto output = builder.appendBulk<render::TransformWriteEntry>(
                        config_.operations.id<render::TransformBatchOp>(), transforms_.size()
                    );
                    std::copy(transforms_.begin(), transforms_.end(), output.begin());
                }
                prepared_ = true;
                if (!builder.valid())
                {
                    return ERenderSyncPrepareResult::FAILED;
                }
                return builder.commandCount() == initial_commands ? ERenderSyncPrepareResult::PREPARED_NO_COMMANDS
                                                                  : ERenderSyncPrepareResult::PREPARED_COMMANDS;
            }

            Mesh3DRenderStageConfig config_;
            MeshChanges changes_;
            MeshMembershipLeaves membership_leaves_;
            MeshStateLeaves state_leaves_;
            std::vector<MeshDeparture> departures_;
            std::vector<MeshStateUpdate> state_updates_;
            std::vector<Entity> state_removals_;
            std::vector<render::TransformWriteEntry> transforms_;
            bool force_full_sync_{false};
            bool prepared_{false};
            bool suppress_state_departure_{false};
            bool allocation_failed_{false};
        };

        struct LightRenderState final
        {
            const void* owner{};
            render::RenderEntityId entity{};
            render::UpsertLightPayload payload{};
            bool published{false};
        };
        static_assert(std::is_nothrow_copy_assignable_v<LightRenderState>);

        struct LightDeparture final
        {
            render::RenderEntityId entity{};
            Entity source{simulation::ecs::NullEntity};
            bool was_published{false};
        };

        struct LightStateUpdate final
        {
            Entity entity{simulation::ecs::NullEntity};
            LightRenderState state{};
        };

        using LightChanges = simulation::ecs::ExtractionChangeSet<
            simulation::ecs::Light3D,
            ComponentList<simulation::ecs::WorldTransform3D>,
            ComponentList<>>;
        using LightMembershipLeaves = simulation::ecs::ComponentSetLeaveObserver<
            simulation::ecs::Light3D,
            ComponentList<simulation::ecs::WorldTransform3D>,
            ComponentList<>>;
        using LightStateLeaves = simulation::ecs::ComponentSetLeaveObserver<
            LightRenderState,
            ComponentList<>,
            ComponentList<>>;

        [[nodiscard]] bool sameLight(
            const render::UpsertLightPayload& left,
            const render::UpsertLightPayload& right
        ) noexcept
        {
            return left.scene_id == right.scene_id && left.entity == right.entity &&
                left.transition_milliseconds == right.transition_milliseconds && left.light_type == right.light_type &&
                std::equal(std::begin(left.spatial_position.page_delta), std::end(left.spatial_position.page_delta),
                    std::begin(right.spatial_position.page_delta)) &&
                std::equal(std::begin(left.spatial_position.local), std::end(left.spatial_position.local),
                    std::begin(right.spatial_position.local)) &&
                std::equal(std::begin(left.direction), std::end(left.direction), std::begin(right.direction)) &&
                std::equal(std::begin(left.color), std::end(left.color), std::begin(right.color)) &&
                left.intensity == right.intensity && left.range == right.range &&
                left.attenuation_constant == right.attenuation_constant &&
                left.attenuation_linear == right.attenuation_linear &&
                left.attenuation_quadratic == right.attenuation_quadratic &&
                left.inner_cone_angle == right.inner_cone_angle && left.outer_cone_angle == right.outer_cone_angle &&
                left.flags == right.flags && left.shadow_map_size == right.shadow_map_size &&
                left.shadow_bias == right.shadow_bias && left.shadow_normal_bias == right.shadow_normal_bias &&
                left.cascade_count == right.cascade_count &&
                std::equal(std::begin(left.cascade_splits), std::end(left.cascade_splits),
                    std::begin(right.cascade_splits)) &&
                std::equal(std::begin(left.area_size), std::end(left.area_size), std::begin(right.area_size));
        }

        class Light3DRenderStage final : public RenderSyncStage
        {
        public:
            explicit Light3DRenderStage(Light3DRenderStageConfig value) : config_(std::move(value))
            {
                using namespace entt::literals;
                changes_.attach(*config_.registry, "scene.light3d.render.changes"_hs, [](auto& storage) {
                    storage.template on_construct<simulation::ecs::Light3D>()
                        .template on_update<simulation::ecs::Light3D>()
                        .template on_construct<simulation::ecs::WorldTransform3D>()
                        .template on_update<simulation::ecs::WorldTransform3D>();
                });
                membership_leaves_.attach(*config_.registry, this, &Light3DRenderStage::onMembershipLeft);
                state_leaves_.attach(*config_.registry, this, &Light3DRenderStage::onStateDestroyed);
            }

            ~Light3DRenderStage() noexcept override
            {
                changes_.detach();
                membership_leaves_.detach();
                state_leaves_.detach();
                suppress_state_departure_ = true;
                while (true)
                {
                    auto states = config_.registry->view<LightRenderState>();
                    const auto found = std::find_if(states.begin(), states.end(), [this](Entity entity) {
                        return config_.registry->get<LightRenderState>(entity).owner == this;
                    });
                    if (found == states.end())
                    {
                        break;
                    }
                    config_.registry->remove<LightRenderState>(*found);
                }
            }

            [[nodiscard]] bool hasPendingChanges() const noexcept override
            {
                return force_full_sync_ || allocation_failed_ || !departures_.empty() || !changes_.empty();
            }

            void requestFullSync() noexcept override
            {
                force_full_sync_ = true;
            }

            [[nodiscard]] ERenderSyncPrepareResult prepare(render::RenderProgramBuilder<>& builder) noexcept override
            {
                discardPrepared();
                if (allocation_failed_)
                {
                    return ERenderSyncPrepareResult::FAILED;
                }
                try
                {
                    return prepareImpl(builder);
                }
                catch (const std::bad_alloc&)
                {
                    discardPrepared();
                    return ERenderSyncPrepareResult::FAILED;
                }
            }

            // LUX_RENDER_COMMIT_LIGHT_BEGIN
            void commitPrepared() noexcept override
            {
                if (!prepared_)
                {
                    return;
                }
                suppress_state_departure_ = true;
                for (const auto entity : state_removals_)
                {
                    if (config_.registry->valid(entity))
                    {
                        const auto* state = config_.registry->try_get<LightRenderState>(entity);
                        if (state != nullptr && state->owner == this)
                        {
                            config_.registry->remove<LightRenderState>(entity);
                        }
                    }
                }
                for (auto& update : state_updates_)
                {
                    config_.registry->get<LightRenderState>(update.entity) = update.state;
                }
                suppress_state_departure_ = false;
                changes_.clear();
                departures_.clear();
                force_full_sync_ = false;
                discardPrepared();
            }
            // LUX_RENDER_COMMIT_LIGHT_END

            void discardPrepared() noexcept override
            {
                state_updates_.clear();
                state_removals_.clear();
                payloads_.clear();
                prepared_ = false;
            }

        private:
            static void onMembershipLeft(void* user, Entity entity) noexcept
            {
                static_cast<Light3DRenderStage*>(user)->recordMembershipDeparture(entity);
            }

            static void onStateDestroyed(void* user, Entity entity) noexcept
            {
                static_cast<Light3DRenderStage*>(user)->recordStateDeparture(entity);
            }

            void recordMembershipDeparture(Entity entity) noexcept
            {
                const auto* state = config_.registry->try_get<LightRenderState>(entity);
                if (state != nullptr && state->owner == this)
                {
                    recordDeparture(LightDeparture{state->entity, entity, state->published});
                }
            }

            void recordStateDeparture(Entity entity) noexcept
            {
                if (suppress_state_departure_)
                {
                    return;
                }
                const auto* state = config_.registry->try_get<LightRenderState>(entity);
                if (state != nullptr && state->owner == this)
                {
                    recordDeparture(LightDeparture{state->entity, entity, state->published});
                }
            }

            void recordDeparture(LightDeparture departure) noexcept
            {
                try
                {
                    departures_.push_back(departure);
                }
                catch (const std::bad_alloc&)
                {
                    allocation_failed_ = true;
                }
            }

            [[nodiscard]] bool membershipRestored(const LightDeparture& departure) const noexcept
            {
                if (!config_.registry->valid(departure.source) || toRenderEntity(departure.source) != departure.entity)
                {
                    return false;
                }
                return config_.registry->all_of<simulation::ecs::Light3D, simulation::ecs::WorldTransform3D>(
                    departure.source
                );
            }

            [[nodiscard]] bool makePayload(Entity entity, render::UpsertLightPayload& payload) const noexcept
            {
                const auto& light = config_.registry->get<simulation::ecs::Light3D>(entity).value;
                const auto& world = config_.registry->get<simulation::ecs::WorldTransform3D>(entity);
                payload.scene_id = config_.scene;
                payload.entity = toRenderEntity(entity);
                switch (light.type)
                {
                case rdesc::ELightType::DIRECTIONAL:
                    payload.light_type = 0U;
                    break;
                case rdesc::ELightType::POINT:
                    payload.light_type = 1U;
                    break;
                case rdesc::ELightType::SPOT:
                    payload.light_type = 2U;
                    break;
                case rdesc::ELightType::AREA:
                    payload.light_type = 3U;
                    break;
                }
                if (!encodePosition(
                        world.value.translation(),
                        config_.coordinate_page_size,
                        config_.scene_origin_page,
                        payload.spatial_position))
                {
                    return false;
                }
                Eigen::Vector3d direction = world.value.linear() * -Eigen::Vector3d::UnitY();
                if (!direction.allFinite() || direction.squaredNorm() <= 0.0)
                {
                    return false;
                }
                direction.normalize();
                for (std::size_t axis = 0U; axis < 3U; ++axis)
                {
                    payload.direction[axis] = static_cast<float>(direction[axis]);
                    payload.color[axis] = light.color[axis];
                }
                payload.intensity = light.intensity;
                payload.range = light.range;
                payload.attenuation_constant = light.attenuation_constant;
                payload.attenuation_linear = light.attenuation_linear;
                payload.attenuation_quadratic = light.attenuation_quadratic;
                payload.inner_cone_angle = light.inner_cone_angle;
                payload.outer_cone_angle = light.outer_cone_angle;
                payload.area_size[0] = light.area_size[0];
                payload.area_size[1] = light.area_size[1];
                payload.flags = light.cast_shadow ? render::LIGHT_FLAG_CAST_SHADOW : 0U;
                payload.shadow_map_size = light.shadow_map_size;
                payload.shadow_bias = light.shadow_bias;
                payload.shadow_normal_bias = light.shadow_normal_bias;
                payload.cascade_count = light.cascade_count;
                std::copy(light.cascade_splits.begin(), light.cascade_splits.end(), payload.cascade_splits);
                return true;
            }

            [[nodiscard]] bool prepareEntity(Entity entity)
            {
                render::UpsertLightPayload payload{};
                if (!makePayload(entity, payload))
                {
                    return false;
                }
                auto* state_slot = config_.registry->try_get<LightRenderState>(entity);
                if (state_slot == nullptr)
                {
                    state_slot = &config_.registry->emplace<LightRenderState>(
                        entity,
                        LightRenderState{.owner = this, .entity = payload.entity}
                    );
                }
                if (state_slot->owner != this)
                {
                    return false;
                }
                const auto* published = state_slot->published ? state_slot : nullptr;
                if (force_full_sync_ || published == nullptr || !sameLight(published->payload, payload))
                {
                    payloads_.push_back(payload);
                }
                if (published == nullptr || !sameLight(published->payload, payload))
                {
                    state_updates_.push_back(
                        LightStateUpdate{entity, LightRenderState{this, payload.entity, payload, true}}
                    );
                }
                return true;
            }

            [[nodiscard]] ERenderSyncPrepareResult prepareImpl(render::RenderProgramBuilder<>& builder)
            {
                const std::size_t initial_commands = builder.commandCount();
                coalesceDepartures(departures_);
                for (const auto& departure : departures_)
                {
                    if (membershipRestored(departure))
                    {
                        continue;
                    }
                    if (departure.was_published)
                    {
                        builder.push(
                            render::opcode_of_v<render::RemoveLightOp>,
                            config_.operations.id<render::RemoveLightOp>(),
                            render::RemoveLightPayload{config_.scene, departure.entity, 0U}
                        );
                    }
                    state_removals_.push_back(departure.source);
                }

                if (force_full_sync_)
                {
                    auto current = componentView<simulation::ecs::Light3D>(
                        *config_.registry,
                        ComponentList<simulation::ecs::WorldTransform3D>{},
                        ComponentList<>{}
                    );
                    for (const auto entity : current)
                    {
                        if (!prepareEntity(entity))
                        {
                            return ERenderSyncPrepareResult::FAILED;
                        }
                    }
                }
                else
                {
                    for (const auto entity : changes_.view())
                    {
                        if (!prepareEntity(entity))
                        {
                            return ERenderSyncPrepareResult::FAILED;
                        }
                    }
                }

                if (!payloads_.empty())
                {
                    auto output = builder.appendBulk<render::UpsertLightPayload>(
                        config_.operations.id<render::LightBatchOp>(), payloads_.size()
                    );
                    std::copy(payloads_.begin(), payloads_.end(), output.begin());
                }
                prepared_ = true;
                if (!builder.valid())
                {
                    return ERenderSyncPrepareResult::FAILED;
                }
                return builder.commandCount() == initial_commands ? ERenderSyncPrepareResult::PREPARED_NO_COMMANDS
                                                                  : ERenderSyncPrepareResult::PREPARED_COMMANDS;
            }

            Light3DRenderStageConfig config_;
            LightChanges changes_;
            LightMembershipLeaves membership_leaves_;
            LightStateLeaves state_leaves_;
            std::vector<LightDeparture> departures_;
            std::vector<LightStateUpdate> state_updates_;
            std::vector<Entity> state_removals_;
            std::vector<render::UpsertLightPayload> payloads_;
            bool force_full_sync_{false};
            bool prepared_{false};
            bool suppress_state_departure_{false};
            bool allocation_failed_{false};
        };

        [[nodiscard]] bool valid(const Mesh3DRenderStageConfig& config) noexcept
        {
            return config.registry != nullptr && !config.scene.isNull() && config.operations.valid() &&
                std::isfinite(config.coordinate_page_size) && config.coordinate_page_size > 0.0;
        }

        [[nodiscard]] bool valid(const Light3DRenderStageConfig& config) noexcept
        {
            return config.registry != nullptr && !config.scene.isNull() && config.operations.valid() &&
                std::isfinite(config.coordinate_page_size) && config.coordinate_page_size > 0.0;
        }
    } // namespace

    lux::cxx::expected<std::unique_ptr<RenderSyncStage>, RenderSyncStageFailure>
    createMesh3DRenderStage(Mesh3DRenderStageConfig config) noexcept
    {
        if (!valid(config))
        {
            return lux::cxx::unexpected(RenderSyncStageFailure{ERenderSyncStageError::INVALID_CONFIGURATION});
        }
        try
        {
            return std::unique_ptr<RenderSyncStage>{new Mesh3DRenderStage{std::move(config)}};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(RenderSyncStageFailure{ERenderSyncStageError::ALLOCATION_FAILURE});
        }
    }

    lux::cxx::expected<std::unique_ptr<RenderSyncStage>, RenderSyncStageFailure>
    createLight3DRenderStage(Light3DRenderStageConfig config) noexcept
    {
        if (!valid(config))
        {
            return lux::cxx::unexpected(RenderSyncStageFailure{ERenderSyncStageError::INVALID_CONFIGURATION});
        }
        try
        {
            return std::unique_ptr<RenderSyncStage>{new Light3DRenderStage{std::move(config)}};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(RenderSyncStageFailure{ERenderSyncStageError::ALLOCATION_FAILURE});
        }
    }
} // namespace lux::scene
