#pragma once
/**
 * @file StaticCollider3DSystem.hpp
 * @brief ECS-first ContentBlobRef to private Jolt static-collider leaf.
 */

#include <lux/engine/ecs/physics3d/systems/Physics3DScene.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DPrepareService.hpp>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DBindingComponent.hpp>
#include <lux/engine/runtime/spatial3d/physics/visibility.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

namespace lux::exec
{
    class AsyncRuntime;
    class AsyncScope;
}

namespace lux::runtime::spatial3d
{
    struct StaticCollider3DSystemConfig final
    {
        /// Global main-owner adoption budget. Expensive blob decoding and
        /// immutable Jolt shape construction happen on the background CPU
        /// service; update creates at most this many backend bodies.
        std::uint32_t maximum_staged_bodies_per_update{4u};
        /// Hard owner/table bound, not a reserve hint.
        std::uint32_t maximum_tracked_batches{256u};
        /// One unit is either one hidden backend body or one unadopted
        /// prepared shape owner.
        std::uint32_t maximum_retirement_units_per_update{8u};
    };

    struct StaticCollider3DSystemSnapshot final
    {
        std::uint64_t preparation_attempts{0u};
        std::uint64_t successful_publications{0u};
        std::uint64_t failed_preparations{0u};
        std::uint64_t stale_publications{0u};
        std::uint64_t stale_completions{0u};
        std::uint64_t queue_backpressure{0u};
        std::uint64_t immediate_hides{0u};
        std::uint64_t retirement_enqueues{0u};
        std::uint64_t retired_batches{0u};
        std::uint64_t coalesced_changes{0u};
        std::uint64_t observer_overflows{0u};
        std::uint64_t capacity_rejections{0u};
        std::uint64_t command_backpressure{0u};
        std::uint64_t owned_budget_bytes{0u};
        std::uint64_t retirement_body_count{0u};
        std::uint64_t retirement_unit_count{0u};
        std::uint32_t tracked_entities{0u};
        std::uint32_t waiting_entities{0u};
        std::uint32_t background_entities{0u};
        std::uint32_t staging_entities{0u};
        std::uint32_t ready_entities{0u};
        std::uint32_t active_entities{0u};
        std::uint32_t failed_entities{0u};
        std::uint32_t retirement_queue_size{0u};
        bool closing{false};
        bool closed{false};
    };

    struct Physics3DSceneSnapshot final
    {
        std::uint32_t dynamic_body_count{0u};
        std::uint32_t character_count{0u};
        std::uint32_t static_heightfield_body_count{0u};
        std::uint64_t capacity_bytes{0u};
        std::uint32_t allocation_count{0u};
        std::uint64_t dropped_contact_facts{0u};
    };

    /// Narrow SceneServices value for diagnostics and gameplay queries.  The
    /// shared scene is the physics fact source; it does not route Section or
    /// partition semantics.
    struct LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_PUBLIC
        Physics3DSceneService final
    {
        std::shared_ptr<lux::ecs::Physics3DScene> scene;

        [[nodiscard]] Physics3DSceneSnapshot snapshot() const noexcept;
    };

    class LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_PUBLIC
        StaticCollider3DSystem final : public lux::ecs::ISystem
    {
      public:
        StaticCollider3DSystem(
            lux::exec::AsyncRuntime& async_runtime,
            lux::exec::AsyncScope& scene_scope,
            StaticCollider3DPrepareClient preparation,
            std::shared_ptr<lux::ecs::Physics3DScene> scene,
            lux::runtime::entity_scene::ContentBlobClient content,
            StaticCollider3DSystemConfig config = {});
        ~StaticCollider3DSystem() override;

        StaticCollider3DSystem(const StaticCollider3DSystem&) = delete;
        StaticCollider3DSystem& operator=(
            const StaticCollider3DSystem&) = delete;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void onRemoved(
            const lux::ecs::SystemRemovalContext& removal) override;
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        void update(const lux::ecs::SystemUpdateContext& context) override;
        [[nodiscard]] std::span<const Type>
        prerequisites() const noexcept override;
        [[nodiscard]] std::span<const Type>
        runsBefore() const noexcept override;

        [[nodiscard]] StaticCollider3DSystemSnapshot
        snapshot() const noexcept;
        [[nodiscard]] std::optional<StaticCollider3DStatusComponent>
        status(entt::entity entity) const noexcept;
        void requestClose() noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;

      private:
        friend struct StaticCollider3DIntentCommand;
        [[nodiscard]] static std::shared_ptr<StaticCollider3DBinding>
        makeBinding(
            std::uint64_t generation,
            std::shared_ptr<lux::ecs::Physics3DScene> scene,
            lux::runtime::entity_scene::ContentBlobLease content,
            StaticCollider3DPrepareBudgetLease budget,
            std::unique_ptr<lux::ecs::Physics3DStaticBatchLease> physics)
            noexcept;
        static void publishBinding(
            const std::shared_ptr<StaticCollider3DBinding>& binding)
            noexcept;
        void applyPublish(
            lux::ecs::Registry& registry,
            entt::entity entity,
            std::uint64_t generation) noexcept;
        void applyArm(
            lux::ecs::Registry& registry,
            entt::entity entity,
            std::uint64_t generation) noexcept;
        void applyStatus(
            lux::ecs::Registry& registry,
            entt::entity entity,
            std::uint64_t generation,
            EStaticCollider3DState state,
            EStaticCollider3DFailure failure) noexcept;
        void applyRemove(
            lux::ecs::Registry& registry,
            entt::entity entity,
            std::uint64_t generation) noexcept;
        void acceptPreparation(
            entt::entity entity,
            std::uint64_t generation,
            lux::async::OperationOutcome<BuildStaticCollider3D> outcome) noexcept;
        void acceptPreparationStopped(
            entt::entity entity,
            std::uint64_t generation) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    enum class EStaticCollider3DIntentAction : std::uint8_t
    {
        ARM,
        PUBLISH,
        STATUS,
        REMOVE
    };

    struct StaticCollider3DIntentCommand final
    {
        using Producer = StaticCollider3DSystem;
        EStaticCollider3DIntentAction action{
            EStaticCollider3DIntentAction::REMOVE};
        entt::entity entity{entt::null};
        std::uint64_t generation{0u};
        EStaticCollider3DState state{
            EStaticCollider3DState::WAITING_CONTENT};
        EStaticCollider3DFailure failure{EStaticCollider3DFailure::NONE};

        [[nodiscard]] std::size_t registryPublicationBytes() const noexcept
        {
            return lux::ecs::ecsCommandSparsePublicationBytes(2u);
        }
        void prepareRegistryPublication(
            lux::ecs::Registry& registry) const noexcept
        {
            lux::ecs::reserveEcsCommandStorage(
                registry.storage<StaticCollider3DBindingComponent>(), 1u);
            lux::ecs::reserveEcsCommandStorage(
                registry.storage<StaticCollider3DStatusComponent>(), 1u);
        }

        void apply(
            lux::ecs::Registry& registry,
            StaticCollider3DSystem& system) const noexcept
        {
            switch (action)
            {
            case EStaticCollider3DIntentAction::ARM:
                system.applyArm(registry, entity, generation);
                break;
            case EStaticCollider3DIntentAction::PUBLISH:
                system.applyPublish(registry, entity, generation);
                break;
            case EStaticCollider3DIntentAction::STATUS:
                system.applyStatus(
                    registry, entity, generation, state, failure);
                break;
            case EStaticCollider3DIntentAction::REMOVE:
                system.applyRemove(registry, entity, generation);
                break;
            }
        }
    };

    static_assert(std::is_trivially_copyable_v<
        StaticCollider3DIntentCommand>);
} // namespace lux::runtime::spatial3d
