#pragma once
/**
 * @file SpatialInterest2DSystem.hpp
 * @brief ECS adapter from 2D interest facts to dimension-neutral demand.
 */

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/math/Grid.hpp>
#include <lux/engine/runtime/spatial2d/infinite/Spatial2DSectionIndex.hpp>
#include <lux/engine/runtime/spatial2d/infinite/visibility.h>
#include <lux/engine/runtime/spatial_partition/SpatialDemand.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace lux::runtime::spatial_partition
{
    class SpatialPartitionSystem;
}

namespace lux::runtime::spatial2d
{
    struct SpatialInterest2DConfig final
    {
        double section_world_size{64.0};
        lux::scene::DemandChannelId channel{
            "lux.spatial2d.resident"};
        std::uint32_t resident_priority{1u};
        std::size_t maximum_sources{8u};

        [[nodiscard]] bool valid() const noexcept;
    };

    enum class ESpatialInterest2DError : std::uint8_t
    {
        INVALID_CONFIG,
        SOURCE_LIMIT,
        INDEX_FAILURE,
        GENERATION_EXHAUSTED,
        PARTITION_REJECTED
    };

    struct SpatialInterest2DFailure final
    {
        ESpatialInterest2DError code{
            ESpatialInterest2DError::INVALID_CONFIG};
        entt::entity entity{entt::null};
        std::optional<Spatial2DIndexFailure> index;
        std::optional<
            lux::runtime::spatial_partition::SpatialPartitionFailure>
            partition;
    };

    struct SpatialInterest2DSnapshot final
    {
        std::size_t tracked_sources{0u};
        std::size_t active_sections{0u};
        std::size_t resident_sections{0u};
        std::uint64_t committed_updates{0u};
        std::uint64_t committed_removals{0u};
        std::uint64_t unchanged_frames{0u};
        std::uint64_t rejected_updates{0u};
        bool closing{false};
        bool closed{false};
        std::optional<SpatialInterest2DFailure> last_failure;
    };

    /// A small-source-count ECS system.  It scans only interest entities, not
    /// scene content, and updates the generic SpatialPartitionSystem when a
    /// source crosses a Section boundary or changes priority.
    class LUX_ENGINE_RUNTIME_SPATIAL2D_INFINITE_PUBLIC
    SpatialInterest2DSystem final : public lux::ecs::ISystem
    {
    public:
        SpatialInterest2DSystem(
            lux::runtime::spatial_partition::SpatialPartitionSystem&
                partition,
            Spatial2DSectionSource source,
            SpatialInterest2DConfig config = {});
        ~SpatialInterest2DSystem() override;

        SpatialInterest2DSystem(const SpatialInterest2DSystem&) = delete;
        SpatialInterest2DSystem& operator=(
            const SpatialInterest2DSystem&) = delete;

        void requestClose() noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;
        [[nodiscard]] SpatialInterest2DSnapshot snapshot() const;
        /// Dimension-leaf consumers use this explicit activity view. Generic
        /// SpatialDemand priority is never interpreted as a domain
        /// visibility/simulation flag.
        [[nodiscard]] bool isActive(
            lux::math::GridCoord2i64 coordinate) const noexcept;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void update(const lux::ecs::SystemUpdateContext& context) override;
        [[nodiscard]] std::span<const Type> prerequisites() const noexcept
            override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept
            override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
