#include <lux/engine/scene/RenderSystem.hpp>

#include <lux/engine/function/render/client/RenderClient.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <entt/entity/entity.hpp>

#include <atomic>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <vector>

namespace lux::scene
{
    namespace
    {
        inline constexpr std::uint32_t kMeshDirty = 1U << 0U;
        inline constexpr std::uint32_t kLightDirty = 1U << 1U;
        inline constexpr std::uint32_t kTransformDirty = 1U << 2U;
        inline constexpr std::uint8_t kMeshPublished = 1U << 0U;
        inline constexpr std::uint8_t kLightPublished = 1U << 1U;
        inline constexpr std::uint64_t kNullEntityBits = simulation::ecs::entityBits(simulation::ecs::NullEntity);

        struct DirtySlot final
        {
            std::atomic<std::uint64_t> observed_entity_bits{kNullEntityBits};
            std::atomic<std::uint32_t> mask{0U};
            std::uint64_t published_entity_bits{kNullEntityBits};
            std::uint8_t published_domains{0U};
        };

        [[nodiscard]] bool encodePosition(
            const Eigen::Vector3d& position,
            double page_size,
            const std::array<std::int64_t, 3>& origin,
            render::RenderLargePosition3D& output
        ) noexcept
        {
            if (!position.allFinite() || !std::isfinite(page_size) || page_size <= 0.0)
            {
                return false;
            }
            for (std::size_t axis = 0U; axis < 3U; ++axis)
            {
                const long double absolute = static_cast<long double>(position[axis]);
                const long double size = static_cast<long double>(page_size);
                const long double page_value = std::floor(absolute / size);
                const bool is_page_out_of_range = page_value < std::numeric_limits<std::int64_t>::min() ||
                    page_value > std::numeric_limits<std::int64_t>::max();
                if (is_page_out_of_range)
                {
                    return false;
                }
                const auto page = static_cast<std::int64_t>(page_value);
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
    }

    struct RenderSystem::Impl final
    {
        explicit Impl(simulation::ecs::Registry& registry, Config value)
            : registry(&registry), config(std::move(value)), capacity(config.expected_entity_capacity),
              slot_count(capacity), summary_count((capacity + 63U) / 64U), slots(std::make_unique<DirtySlot[]>(slot_count)),
              summary(std::make_unique<std::atomic<std::uint64_t>[]>(summary_count)), updates(1U)
        {
            export_indices.reserve(capacity);
            next_bits.reserve(capacity);
            next_domains.reserve(capacity);
            light_payloads.reserve(capacity);
            mesh_construct = registry.on_construct<simulation::ecs::Mesh3D>().connect<&Impl::onMesh>(*this);
            mesh_update = registry.on_update<simulation::ecs::Mesh3D>().connect<&Impl::onMesh>(*this);
            mesh_destroy = registry.on_destroy<simulation::ecs::Mesh3D>().connect<&Impl::onMesh>(*this);
            light_construct = registry.on_construct<simulation::ecs::Light3D>().connect<&Impl::onLight>(*this);
            light_update = registry.on_update<simulation::ecs::Light3D>().connect<&Impl::onLight>(*this);
            light_destroy = registry.on_destroy<simulation::ecs::Light3D>().connect<&Impl::onLight>(*this);
            transform_construct =
                registry.on_construct<simulation::ecs::WorldTransform3D>().connect<&Impl::onTransform>(*this);
            transform_update = registry.on_update<simulation::ecs::WorldTransform3D>().connect<&Impl::onTransform>(*this);
            transform_destroy =
                registry.on_destroy<simulation::ecs::WorldTransform3D>().connect<&Impl::onTransform>(*this);

            for (const auto entity : registry.view<simulation::ecs::Mesh3D>()) mark(entity, kMeshDirty);
            for (const auto entity : registry.view<simulation::ecs::Light3D>()) mark(entity, kLightDirty);
            for (const auto entity : registry.view<simulation::ecs::WorldTransform3D>()) mark(entity, kTransformDirty);
        }

        void onMesh(simulation::ecs::Registry&, simulation::ecs::Entity entity) noexcept
        {
            mark(entity, kMeshDirty);
        }

        void onLight(simulation::ecs::Registry&, simulation::ecs::Entity entity) noexcept
        {
            mark(entity, kLightDirty);
        }

        void onTransform(simulation::ecs::Registry&, simulation::ecs::Entity entity) noexcept
        {
            mark(entity, kTransformDirty);
        }

        void mark(simulation::ecs::Entity entity, std::uint32_t dirty) noexcept
        {
            const std::size_t index = static_cast<std::size_t>(entt::to_entity(entity));
            if (index >= slot_count)
            {
                overflow.store(true, std::memory_order_release);
                return;
            }
            auto& slot = slots[index];
            slot.observed_entity_bits.store(simulation::ecs::entityBits(entity), std::memory_order_release);
            const auto previous = slot.mask.fetch_or(dirty, std::memory_order_acq_rel);
            if (previous == 0U)
            {
                summary[index / 64U].fetch_or(1ULL << (index % 64U), std::memory_order_release);
            }
        }

        void collectDirty() noexcept
        {
            export_indices.clear();
            for (std::size_t word_index = 0U; word_index < summary_count; ++word_index)
            {
                auto word = summary[word_index].load(std::memory_order_acquire);
                while (word != 0U)
                {
                    const auto bit = static_cast<std::size_t>(std::countr_zero(word));
                    const auto index = word_index * 64U + bit;
                    if (index < slot_count)
                    {
                        export_indices.push_back(index);
                    }
                    word &= word - 1U;
                }
            }
        }

        [[nodiscard]] bool appendMeshUpsert(
            render::RenderProgramBuilder<>& builder,
            simulation::ecs::Entity entity,
            const simulation::ecs::Mesh3D& mesh,
            const simulation::ecs::WorldTransform3D& world
        ) noexcept
        {
            render::UpsertMeshInstancePayload payload{};
            payload.scene_id = config.scene;
            payload.entity = toRenderEntity(entity);
            payload.mesh_asset = mesh.value.mesh;
            payload.material_asset = mesh.value.material;
            if (!encodeTransform(world, config.coordinate_page_size, config.scene_origin_page, payload.transform))
            {
                return false;
            }
            payload.flags = (mesh.value.visible ? render::kInstanceFlagVisible : 0U) |
                (mesh.value.cast_shadow ? render::kInstanceFlagCastShadow : 0U) |
                (mesh.value.receive_shadow ? render::kInstanceFlagReceiveShadow : 0U);
            builder.push(
                render::opcode_of_v<render::UpsertMeshInstanceOp>,
                config.mesh_stack.id<render::UpsertMeshInstanceOp>(),
                payload
            );
            return builder.valid();
        }

        [[nodiscard]] bool appendLightUpsert(
            simulation::ecs::Entity entity,
            const simulation::ecs::Light3D& light,
            const simulation::ecs::WorldTransform3D& world
        ) noexcept
        {
            render::UpsertLightPayload payload{};
            payload.scene_id = config.scene;
            payload.entity = toRenderEntity(entity);
            switch (light.value.type)
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
                    world.value.translation(), config.coordinate_page_size, config.scene_origin_page,
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
                payload.color[axis] = light.value.color[axis];
            }
            payload.intensity = light.value.intensity;
            payload.range = light.value.range;
            payload.attenuation_constant = light.value.attenuation_constant;
            payload.attenuation_linear = light.value.attenuation_linear;
            payload.attenuation_quadratic = light.value.attenuation_quadratic;
            payload.inner_cone_angle = light.value.inner_cone_angle;
            payload.outer_cone_angle = light.value.outer_cone_angle;
            payload.area_size[0] = light.value.area_size[0];
            payload.area_size[1] = light.value.area_size[1];
            payload.flags = light.value.cast_shadow ? render::LIGHT_FLAG_CAST_SHADOW : 0U;
            payload.shadow_map_size = light.value.shadow_map_size;
            payload.shadow_bias = light.value.shadow_bias;
            payload.shadow_normal_bias = light.value.shadow_normal_bias;
            payload.cascade_count = light.value.cascade_count;
            std::copy(light.value.cascade_splits.begin(), light.value.cascade_splits.end(), payload.cascade_splits);
            light_payloads.push_back(payload);
            return true;
        }

        simulation::ecs::Registry* registry{};
        Config config{};
        std::size_t capacity{};
        std::size_t slot_count{};
        std::size_t summary_count{};
        std::unique_ptr<DirtySlot[]> slots;
        std::unique_ptr<std::atomic<std::uint64_t>[]> summary;
        std::atomic<bool> overflow{false};
        std::vector<std::size_t> export_indices;
        std::vector<std::uint64_t> next_bits;
        std::vector<std::uint8_t> next_domains;
        std::vector<render::UpsertLightPayload> light_payloads;
        lux::cxx::BoundedSpscFrameRing<render::RenderProgram<>, 3> updates;
        bool full_sync_required{true};
        bool forward_pending{false};
        entt::scoped_connection mesh_construct;
        entt::scoped_connection mesh_update;
        entt::scoped_connection mesh_destroy;
        entt::scoped_connection light_construct;
        entt::scoped_connection light_update;
        entt::scoped_connection light_destroy;
        entt::scoped_connection transform_construct;
        entt::scoped_connection transform_update;
        entt::scoped_connection transform_destroy;
    };

    RenderSystem::RenderSystem(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
    }

    RenderSystem::~RenderSystem() noexcept = default;

    lux::cxx::expected<std::unique_ptr<RenderSystem>, RenderSystemFailure>
    RenderSystem::create(simulation::ecs::Registry& registry, Config config) noexcept
    {
        const bool is_invalid = config.scene.isNull() || !config.mesh_stack.valid() || !config.light.valid() ||
            !std::isfinite(config.coordinate_page_size) || config.coordinate_page_size <= 0.0 ||
            config.expected_entity_capacity == 0U;
        if (is_invalid)
        {
            return lux::cxx::unexpected(RenderSystemFailure{ERenderSystemError::InvalidConfiguration});
        }
        try
        {
            return std::unique_ptr<RenderSystem>{new RenderSystem{std::make_unique<Impl>(registry, std::move(config))}};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(RenderSystemFailure{ERenderSystemError::AllocationFailure});
        }
    }

    ERenderPublishResult RenderSystem::tryPublish() noexcept
    {
        auto& state = *impl_;
        if (state.overflow.exchange(false, std::memory_order_acq_rel))
        {
            state.full_sync_required = true;
            return ERenderPublishResult::Failed;
        }
        state.collectDirty();
        const bool was_full_sync = state.full_sync_required;
        if (state.export_indices.empty() && !was_full_sync)
        {
            return ERenderPublishResult::NoChanges;
        }
        auto* program = state.updates.tryBeginWrite();
        if (program == nullptr)
        {
            return ERenderPublishResult::Backpressured;
        }
        render::RenderProgramBuilder<> builder{*program};
        builder.begin(render::ProgramMemoryHints{
            .command_capacity = state.export_indices.size() * 3U + 1U,
            .payload_capacity = state.export_indices.size() * 256U
        });
        program->kind = render::ERenderProgramKind::StateUpdate;
        state.next_bits.clear();
        state.next_domains.clear();
        state.light_payloads.clear();

        for (const auto index : state.export_indices)
        {
            auto& slot = state.slots[index];
            const auto observed = slot.observed_entity_bits.load(std::memory_order_acquire);
            const auto candidate = static_cast<simulation::ecs::Entity>(observed);
            const bool live = state.registry->valid(candidate);
            const auto current_bits = live ? observed : kNullEntityBits;
            auto effective_domains = slot.published_domains;
            if (effective_domains != 0U && slot.published_entity_bits != current_bits)
            {
                if ((effective_domains & kMeshPublished) != 0U)
                {
                    builder.push(
                        render::opcode_of_v<render::RemoveMeshInstanceOp>,
                        state.config.mesh_stack.id<render::RemoveMeshInstanceOp>(),
                        render::RemoveMeshInstancePayload{
                            state.config.scene,
                            static_cast<render::RenderEntityId>(slot.published_entity_bits)
                        }
                    );
                }
                if ((effective_domains & kLightPublished) != 0U)
                {
                    builder.push(
                        render::opcode_of_v<render::RemoveLightOp>,
                        state.config.light.id<render::RemoveLightOp>(),
                        render::RemoveLightPayload{
                            state.config.scene,
                            static_cast<render::RenderEntityId>(slot.published_entity_bits),
                            0U
                        }
                    );
                }
                effective_domains = 0U;
            }

            std::uint8_t current_domains = 0U;
            const auto* world = live ? state.registry->try_get<simulation::ecs::WorldTransform3D>(candidate) : nullptr;
            const auto* mesh = live ? state.registry->try_get<simulation::ecs::Mesh3D>(candidate) : nullptr;
            const auto* light = live ? state.registry->try_get<simulation::ecs::Light3D>(candidate) : nullptr;
            if (world != nullptr && mesh != nullptr)
            {
                if (!state.appendMeshUpsert(builder, candidate, *mesh, *world)) return ERenderPublishResult::Failed;
                current_domains |= kMeshPublished;
            }
            if (world != nullptr && light != nullptr)
            {
                if (!state.appendLightUpsert(candidate, *light, *world)) return ERenderPublishResult::Failed;
                current_domains |= kLightPublished;
            }
            if ((effective_domains & kMeshPublished) != 0U && (current_domains & kMeshPublished) == 0U)
            {
                builder.push(
                    render::opcode_of_v<render::RemoveMeshInstanceOp>,
                    state.config.mesh_stack.id<render::RemoveMeshInstanceOp>(),
                    render::RemoveMeshInstancePayload{state.config.scene, toRenderEntity(candidate)}
                );
            }
            if ((effective_domains & kLightPublished) != 0U && (current_domains & kLightPublished) == 0U)
            {
                builder.push(
                    render::opcode_of_v<render::RemoveLightOp>,
                    state.config.light.id<render::RemoveLightOp>(),
                    render::RemoveLightPayload{state.config.scene, toRenderEntity(candidate), 0U}
                );
            }
            state.next_bits.push_back(current_bits);
            state.next_domains.push_back(current_domains);
        }

        if (!state.light_payloads.empty())
        {
            auto payloads = builder.appendBulk<render::UpsertLightPayload>(
                state.config.light.id<render::LightBatchOp>(), state.light_payloads.size()
            );
            std::copy(state.light_payloads.begin(), state.light_payloads.end(), payloads.begin());
        }
        if (!builder.valid())
        {
            return ERenderPublishResult::Failed;
        }
        if (program->commands.empty() && !was_full_sync)
        {
            for (const auto index : state.export_indices)
            {
                state.slots[index].mask.store(0U, std::memory_order_release);
                state.summary[index / 64U].fetch_and(~(1ULL << (index % 64U)), std::memory_order_acq_rel);
            }
            return ERenderPublishResult::NoChanges;
        }
        if (!state.updates.publishWrite())
        {
            return ERenderPublishResult::Backpressured;
        }
        for (std::size_t offset = 0U; offset < state.export_indices.size(); ++offset)
        {
            const auto index = state.export_indices[offset];
            auto& slot = state.slots[index];
            slot.published_entity_bits = state.next_bits[offset];
            slot.published_domains = state.next_domains[offset];
            slot.mask.store(0U, std::memory_order_release);
            state.summary[index / 64U].fetch_and(~(1ULL << (index % 64U)), std::memory_order_acq_rel);
        }
        state.full_sync_required = false;
        return was_full_sync ? ERenderPublishResult::FullSyncPublished : ERenderPublishResult::Published;
    }

    void RenderSystem::requestFullSync() noexcept
    {
        auto& state = *impl_;
        state.full_sync_required = true;
        for (const auto entity : state.registry->view<simulation::ecs::Mesh3D>()) state.mark(entity, kMeshDirty);
        for (const auto entity : state.registry->view<simulation::ecs::Light3D>()) state.mark(entity, kLightDirty);
    }

    ERenderForwardResult RenderSystem::tryForwardUpdate(render::RenderProgramSession& session) noexcept
    {
        auto& state = *impl_;
        if (session.isStopping()) return ERenderForwardResult::Stopping;
        if (session.hasPendingSubmit() && !session.retryPendingSubmit()) return ERenderForwardResult::Backpressured;
        if (!state.forward_pending)
        {
            if (!state.updates.tryAcquireRead()) return ERenderForwardResult::NoUpdate;
            state.forward_pending = true;
        }
        auto& program = state.updates.currentRead();
        if (!session.trySubmitPrepared(program)) return ERenderForwardResult::Backpressured;
        state.forward_pending = false;
        return ERenderForwardResult::Forwarded;
    }
}
