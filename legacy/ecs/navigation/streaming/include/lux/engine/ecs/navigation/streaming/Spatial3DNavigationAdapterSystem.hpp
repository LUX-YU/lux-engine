#pragma once
/**
 * @file Spatial3DNavigationAdapterSystem.hpp
 * @brief ECS adapter from EntityScene blobs to Navigation3D preparation.
 */

#include <lux/engine/ecs/entity_scene/ContentBlobStorage.hpp>
#include <lux/engine/ecs/navigation/streaming/Navigation3DPreparePort.hpp>
#include <lux/engine/ecs/navigation/streaming/visibility.h>
#include <lux/engine/ecs/navigation/systems/Navigation3DSystem.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace lux::ecs::navigation::streaming
{
    inline constexpr std::size_t kNavigation3DAdapterByteBudget =
        256u * 1024u * 1024u;

    struct Spatial3DNavigationAdapterConfig final
    {
        std::uint32_t maximum_owned_requests{256u};
        std::size_t maximum_owned_bytes{kNavigation3DAdapterByteBudget};
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

    class LUX_ENGINE_ECS_NAVIGATION_STREAMING_PUBLIC
    Spatial3DNavigationAdapterSystem final : public lux::ecs::ISystem
    {
    public:
        Spatial3DNavigationAdapterSystem(
            Navigation3DPrepareClient preparation,
            lux::ecs::Navigation3DSystem& navigation,
            lux::ecs::entity_scene::ContentBlobClient content,
            Spatial3DNavigationAdapterConfig config = {});
        ~Spatial3DNavigationAdapterSystem() override;

        Spatial3DNavigationAdapterSystem(
            const Spatial3DNavigationAdapterSystem&) = delete;
        Spatial3DNavigationAdapterSystem& operator=(
            const Spatial3DNavigationAdapterSystem&) = delete;

        void update(const lux::ecs::SystemUpdateContext& context) override;
        void onRemoved(const lux::ecs::SystemRemovalContext&) override
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
            lux::async::OperationOutcome<BuildNavigationRegion3D>
                outcome) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::ecs::navigation::streaming
