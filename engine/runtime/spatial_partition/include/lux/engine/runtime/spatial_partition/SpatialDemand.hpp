#pragma once
/**
 * @file SpatialDemand.hpp
 * @brief Dimension-neutral EntitySection residency demand values.
 */

#include <lux/cxx/core/StableNameId.hpp>
#include <lux/engine/ecs/scene_format/Identifiers.hpp>
#include <lux/engine/scene/SceneDescription.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lux::runtime::entity_scene
{
    enum class EEntitySectionRequestError : std::uint8_t;
}

namespace lux::runtime::spatial_partition
{
    struct SpatialDemandSourceIdTag final {};
    using SpatialDemandSourceId = lux::cxx::StableNameId<SpatialDemandSourceIdTag>;

    struct SpatialDemandEntry final
    {
        lux::ecs::scene_format::EntitySectionId section;
        std::uint32_t priority{0u};

        friend bool operator==(
            const SpatialDemandEntry&,
            const SpatialDemandEntry&) = default;
    };

    struct SpatialDemandSourceUpdate final
    {
        SpatialDemandSourceId source;
        std::uint64_t generation{0u};
        lux::ecs::scene_format::DemandChannelId channel;
        std::vector<SpatialDemandEntry> demands;
        /// Source-owned records for procedurally addressable Sections. They
        /// participate in the same prospective transaction as `demands` and
        /// disappear when this source is replaced/removed. Two live sources
        /// may publish the same record only when every cooked field matches.
        /// Stored package records remain in EntitySectionRecordStore and do
        /// not need to be repeated here.
        std::vector<lux::ecs::scene_format::SectionRecord> records;
    };

    struct SpatialPartitionBudget final
    {
        std::uint64_t maximum_decoded_bytes{0u};
        std::uint64_t maximum_entities{0u};

        [[nodiscard]] bool valid() const noexcept
        {
            return maximum_decoded_bytes != 0u && maximum_entities != 0u;
        }
    };

    enum class ESpatialPartitionError : std::uint8_t
    {
        INVALID_BUDGET,
        INVALID_SOURCE,
        INVALID_CHANNEL,
        EMPTY_DEMAND,
        DUPLICATE_SECTION,
        INVALID_RECORD,
        SECTION_NOT_FOUND,
        CHANNEL_MISMATCH,
        INVALID_DEPENDENCY,
        INVALID_DYNAMIC_RECORD,
        DYNAMIC_RECORD_CONFLICT,
        DECODED_BYTE_BUDGET_EXCEEDED,
        ENTITY_BUDGET_EXCEEDED,
        BUDGET_OVERFLOW,
        STALE_SOURCE_GENERATION,
        SOURCE_NOT_FOUND,
        PLAN_STALE,
        PLAN_REVISION_EXHAUSTED,
        OWNER_NOT_ADDED,
        LOADER_UNAVAILABLE,
        LOADER_REGISTRY_MISMATCH,
        SECTION_ACQUIRE_FAILED
    };

    struct SpatialPartitionFailure final
    {
        ESpatialPartitionError code{ESpatialPartitionError::INVALID_SOURCE};
        SpatialDemandSourceId source;
        lux::ecs::scene_format::EntitySectionId section;
        std::uint64_t requested{0u};
        std::uint64_t available{0u};
        std::optional<
        lux::runtime::entity_scene::EEntitySectionRequestError>
            loader_error;
    };

    template <typename T>
    using SpatialPartitionExp = lux::cxx::expected<T, SpatialPartitionFailure>;
}
