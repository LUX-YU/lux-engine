#pragma once
/**
 * @file Spatial3DNavigationAdapterSystem.hpp
 * @brief Leaf adapter from EntityScene blobs to typed navigation preparation.
 */

#include <lux/engine/ecs/navigation/systems/Navigation3DSystem.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/spatial3d/navigation/Navigation3DPrepareService.hpp>
#include <lux/engine/runtime/spatial3d/navigation/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::exec
{
    class AsyncRuntime;
    class AsyncScope;
}

namespace lux::runtime::spatial3d
{
    struct Spatial3DNavigationAdapterConfig final
    {
        std::uint32_t maximum_owned_requests{256u};
        std::size_t maximum_owned_bytes{kNavigation3DPrepareByteBudget};
    };

    struct Spatial3DNavigationAdapterSnapshot final
    {
        std::uint64_t admitted_requests{0u};
        std::uint64_t submitted_requests{0u};
        std::uint64_t accepted_completions{0u};
        std::uint64_t queue_backpressure{0u};
        std::uint64_t stale_completions{0u};
        std::uint64_t cancelled_requests{0u};
        std::size_t current_requests{0u};
        std::size_t current_completions{0u};
        std::size_t waiting_admission_requests{0u};
        std::size_t in_flight_requests{0u};
        std::size_t current_bytes{0u};
        bool closing{false};
    };

    /// This leaf is the only production boundary that sees all three of the
    /// ECS owner, EntityScene blob resolver and AsyncRuntime operation.  It
    /// owns a fixed request slot array and never allocates a per-frame request
    /// packet or callback graph.
    class LUX_ENGINE_RUNTIME_SPATIAL3D_NAVIGATION_PUBLIC
        Spatial3DNavigationAdapterSystem final : public lux::ecs::ISystem
    {
      public:
        Spatial3DNavigationAdapterSystem(
            lux::exec::AsyncRuntime& async_runtime,
            lux::exec::AsyncScope& scene_scope,
            Navigation3DPrepareClient preparation,
            lux::ecs::Navigation3DSystem& navigation,
            lux::ecs::entity_scene::ContentBlobClient content,
            Spatial3DNavigationAdapterConfig config = {});
        ~Spatial3DNavigationAdapterSystem() override;

        Spatial3DNavigationAdapterSystem(
            const Spatial3DNavigationAdapterSystem&) = delete;
        Spatial3DNavigationAdapterSystem&
        operator=(const Spatial3DNavigationAdapterSystem&) = delete;

        void update(const lux::ecs::SystemUpdateContext& context) override;
        void onRemoved(
            const lux::ecs::SystemRemovalContext&) override
        {
            requestClose();
        }
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        [[nodiscard]] std::span<const Type>
        prerequisites() const noexcept override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept override;

        [[nodiscard]] Spatial3DNavigationAdapterSnapshot
        snapshot() const noexcept;
        void requestClose() noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;

      private:
        void acceptPreparation(
            std::uint32_t slot,
            std::uint32_t slot_generation,
            lux::async::OperationOutcome<BuildNavigationRegion3D> outcome) noexcept;
        void acceptPreparationStopped(std::uint32_t slot,
                                      std::uint32_t slot_generation) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::runtime::spatial3d
