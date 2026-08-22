#pragma once
/**
 * @file Navigation3DSystem.hpp
 * @brief ECS owner for independently prepared NavigationRegion3D content.
 */

#include <lux/engine/ecs/navigation/components/NavigationRegion3DStatusComponent.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/navigation/detour3d/NavigationDetour3D.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>

namespace lux::ecs
{
    struct NavigationRegion3DPrepareRequest final
    {
        entt::entity entity{entt::null};
        std::uint64_t generation{0u};
        lux::ecs::scene_format::ContentBlobRef content;
    };

    struct Navigation3DSystemSnapshot final
    {
        std::uint64_t generation{0u};
        std::uint64_t requests_emitted{0u};
        std::uint64_t queue_backpressure{0u};
        std::uint64_t stale_completions{0u};
        std::uint64_t failed_regions{0u};
        std::uint64_t staging_work_items{0u};
        std::uint64_t retirement_work_items{0u};
        std::uint64_t staging_bytes{0u};
        std::uint64_t retired_bytes{0u};
        std::uint32_t maximum_staging_work_items_per_tick{0u};
        std::uint32_t maximum_retirement_work_items_per_tick{0u};
        std::uint32_t maximum_close_hides_per_tick{0u};
        std::uint64_t close_hides{0u};
        std::uint64_t owner_bytes{0u};
        std::uint32_t waiting_regions{0u};
        std::uint32_t staging_regions{0u};
        std::uint32_t ready_regions{0u};
        std::uint32_t active_regions{0u};
        std::uint32_t retiring_regions{0u};
    };

    struct Navigation3DSystemConfig final
    {
        std::uint32_t maximum_pending_requests{256u};
        std::uint32_t maximum_pending_completions{256u};
        std::uint32_t maximum_staging_granules_per_tick{1u};
        std::uint32_t maximum_retirement_granules_per_tick{1u};
        std::uint32_t maximum_close_hides_per_tick{1u};
    };

    /// ECS owner for bounded NavigationRegion3D preparation and retirement.
    /// A leaf typed AsyncRuntime adapter
    /// reads the bounded preparation window, resolves ContentBlobRef and
    /// submits prepareNavigationRegion3D() to its background CPU scheduler.
    /// Owning completions return on MainThreadScheduler; registry/backend
    /// publication happens only at the Schedule command barrier.
    class LUX_FUNCTION_PUBLIC Navigation3DSystem final : public ISystem
    {
      public:
        explicit Navigation3DSystem(
            std::shared_ptr<lux::navigation::detour3d::Navigation3DBackend>
                backend,
            Navigation3DSystemConfig config = {});
        ~Navigation3DSystem() override;
        Navigation3DSystem(const Navigation3DSystem&) = delete;
        Navigation3DSystem& operator=(const Navigation3DSystem&) = delete;

        void onAdded(const SystemSetupContext& setup) override;
        void onRemoved(const SystemRemovalContext& removal) override;
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        void update(const SystemUpdateContext& context) override;

        /// Allocation-free producer window.  The span stays valid until the
        /// next update/consume call.  A bounded typed adapter must consume
        /// only requests it has admitted.
        [[nodiscard]] std::span<const NavigationRegion3DPrepareRequest>
        pendingPreparationRequests() const noexcept;
        void consumePreparationRequests(std::size_t count) noexcept;

        /// Main-thread completion adoption. Stale generation/entity results
        /// are rejected without changing live navigation state, but a valid
        /// stale prepared value is consumed into bounded retirement. Capacity
        /// or stale-retirement backpressure returns false without consuming
        /// it, so the adapter can distinguish retry from stale by testing
        /// prepared.valid().
        [[nodiscard]] bool
        acceptPrepared(entt::entity entity,
                       std::uint64_t generation,
                       lux::navigation::detour3d::PreparedNavigationRegion3D&&
                           prepared) noexcept;
        /// Failure values carry no backend owner. True means either queued or
        /// stale-and-consumed; false exclusively means bounded backpressure.
        [[nodiscard]] bool
        acceptFailure(entt::entity entity,
                      std::uint64_t generation,
                      lux::navigation::detour3d::NavigationRegion3DFailure&&
                          failure) noexcept;

        /// Scene-owner-thread query seam. Keeping queries on the Schedule
        /// owner serializes them with retirement; use Navigation3DBackend
        /// directly when an independently owned concurrent backend is needed.
        [[nodiscard]] lux::navigation::NavigationPathResult
        query(const lux::navigation::NavigationPathRequest& request)
            const noexcept;
        /// Scene-owner-thread lifecycle snapshot.
        [[nodiscard]] Navigation3DSystemSnapshot snapshot() const noexcept;
        [[nodiscard]] std::optional<NavigationRegion3DStatusComponent>
        status(entt::entity entity) const noexcept;

        void requestClose() noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;

      private:
        friend struct Navigation3DIntentCommand;
        void applyReconcile(lux::ecs::Registry& registry,
                            entt::entity entity) noexcept;
        void applyPublish(lux::ecs::Registry& registry,
                          entt::entity entity,
                          std::uint64_t generation) noexcept;
        void applyFailure(lux::ecs::Registry& registry,
                          entt::entity entity,
                          std::uint64_t generation,
                          ENavigationRegion3DFailureCode failure) noexcept;
        void requireOwnerThread() const noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    enum class ENavigation3DIntentAction : std::uint8_t
    {
        RECONCILE,
        PUBLISH,
        FAIL
    };

    struct Navigation3DIntentCommand final
    {
        using Producer = Navigation3DSystem;
        ENavigation3DIntentAction action{ENavigation3DIntentAction::RECONCILE};
        entt::entity entity{entt::null};
        std::uint64_t generation{0u};
        ENavigationRegion3DFailureCode failure{
            ENavigationRegion3DFailureCode::NONE};

        [[nodiscard]] std::size_t registryPublicationBytes() const noexcept
        {
            return ecsCommandSparsePublicationBytes(1u);
        }
        void prepareRegistryPublication(
            lux::ecs::Registry& registry) const noexcept
        {
            reserveEcsCommandStorage(
                registry.storage<NavigationRegion3DStatusComponent>(), 1u);
        }

        void apply(lux::ecs::Registry& registry,
                   Navigation3DSystem& system) const noexcept
        {
            switch (action)
            {
            case ENavigation3DIntentAction::RECONCILE:
                system.applyReconcile(registry, entity);
                break;
            case ENavigation3DIntentAction::PUBLISH:
                system.applyPublish(registry, entity, generation);
                break;
            case ENavigation3DIntentAction::FAIL:
                system.applyFailure(registry, entity, generation, failure);
                break;
            }
        }
    };

    static_assert(std::is_trivially_copyable_v<Navigation3DIntentCommand>);
} // namespace lux::ecs
