#pragma once
/**
 * @file SpatialInterest3DSystem.hpp
 * @brief ECS adapter from 3D interest facts to dimension-neutral demand.
 */

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/math/Grid.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/Spatial3DSectionSource.hpp>
#include <lux/engine/runtime/spatial3d/partitioned/visibility.h>
#include <lux/engine/runtime/spatial_partition/SpatialDemand.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lux::runtime::spatial_partition
{
    class SpatialPartitionSystem;
}

namespace lux::runtime::spatial3d
{
    struct SpatialInterest3DBand final
    {
        lux::runtime::spatial_partition::SpatialDemandSourceId
            source_namespace;
        Spatial3DSectionSource sections;
        double cell_world_size{64.0};
        lux::ecs::scene_format::DemandChannelId channel{
            "lux.spatial3d.resident"};
        double active_distance_scale{1.0};
        double resident_distance_scale{1.0};
        std::size_t maximum_sections_per_source{4096u};

        [[nodiscard]] LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_PUBLIC bool
        valid() const noexcept;
    };

    /// One ECS adapter owns every cooked 3D spatial band. The Schedule keeps
    /// one behavior type, while each band has an independent demand-source
    /// namespace, channel, grid and Section catalog.
    struct SpatialInterest3DConfig final
    {
        std::vector<SpatialInterest3DBand> bands;
        std::size_t maximum_sources{8u};

        [[nodiscard]] LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_PUBLIC bool
        valid() const noexcept;
    };

    enum class ESpatialInterest3DError : std::uint8_t
    {
        INVALID_CONFIG,
        INVALID_INTEREST,
        SOURCE_LIMIT,
        SOURCE_FAILURE,
        GENERATION_EXHAUSTED,
        PARTITION_REJECTED
    };

    struct SpatialInterest3DFailure final
    {
        ESpatialInterest3DError code{
            ESpatialInterest3DError::INVALID_CONFIG};
        entt::entity entity{entt::null};
        lux::runtime::spatial_partition::SpatialDemandSourceId band;
        std::optional<Spatial3DSourceFailure> source;
        std::optional<
            lux::runtime::spatial_partition::SpatialPartitionFailure>
            partition;
    };

    struct SpatialInterest3DSnapshot final
    {
        std::size_t tracked_sources{0u};
        std::size_t active_sections{0u};
        std::size_t resident_sections{0u};
        std::uint64_t committed_updates{0u};
        std::uint64_t committed_removals{0u};
        std::uint64_t unchanged_frames{0u};
        std::uint64_t rejected_updates{0u};
        std::size_t maximum_sources{0u};
        std::size_t maximum_sections_per_source{0u};
        bool closing{false};
        bool closed{false};
        std::optional<SpatialInterest3DFailure> last_failure;
    };

    class LUX_ENGINE_RUNTIME_SPATIAL3D_PARTITIONED_PUBLIC
    SpatialInterest3DSystem final : public lux::ecs::ISystem
    {
    public:
        SpatialInterest3DSystem(
            lux::runtime::spatial_partition::SpatialPartitionSystem&
                partition,
            SpatialInterest3DConfig config);
        ~SpatialInterest3DSystem() override;

        SpatialInterest3DSystem(const SpatialInterest3DSystem&) = delete;
        SpatialInterest3DSystem& operator=(
            const SpatialInterest3DSystem&) = delete;

        void requestClose() noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;
        [[nodiscard]] SpatialInterest3DSnapshot snapshot() const;
        [[nodiscard]] bool isActive(
            lux::math::GridCoord3i64 coordinate) const noexcept;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void onRemoved(
            const lux::ecs::SystemRemovalContext& removal) override;
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override;
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
