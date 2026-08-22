#pragma once
/**
 * @file SectionResidencyDemand.hpp
 * @brief Dimension-neutral EntitySection residency demand values.
 */

#include <lux/cxx/core/StableNameId.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/scene_format/SceneSectionManifest.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lux::ecs::entity_scene
{
    enum class EEntitySectionRequestError : std::uint8_t;
}

namespace lux::ecs::entity_scene::residency
{
    struct SectionDemandSourceIdTag final {};
    using SectionDemandSourceId = lux::cxx::StableNameId<SectionDemandSourceIdTag>;

    struct SectionDemandEntry final
    {
        lux::ecs::scene_format::EntitySectionId section;
        std::uint32_t priority{0u};

        friend bool operator==(
            const SectionDemandEntry&,
            const SectionDemandEntry&) = default;
    };

    struct SectionDemandSourceUpdate final
    {
        SectionDemandSourceId source;
        std::uint64_t generation{0u};
        lux::ecs::scene_format::DemandChannelId channel;
        std::vector<SectionDemandEntry> demands;
        /// Source-owned records for procedurally addressable Sections. They
        /// participate in the same prospective transaction as `demands` and
        /// disappear when this source is replaced/removed. Two live sources
        /// may publish the same record only when every cooked field matches.
        /// Stored package records remain in EntitySectionRecordStore and do
        /// not need to be repeated here.
        std::vector<lux::ecs::scene_format::SectionRecord> records;
    };

    struct SectionResidencyBudget final
    {
        std::uint64_t maximum_decoded_bytes{0u};
        std::uint64_t maximum_entities{0u};

        [[nodiscard]] bool valid() const noexcept
        {
            return maximum_decoded_bytes != 0u && maximum_entities != 0u;
        }
    };

    enum class ESectionResidencyError : std::uint8_t
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

    struct SectionResidencyFailure final
    {
        ESectionResidencyError code{ESectionResidencyError::INVALID_SOURCE};
        SectionDemandSourceId source;
        lux::ecs::scene_format::EntitySectionId section;
        std::uint64_t requested{0u};
        std::uint64_t available{0u};
        std::optional<lux::ecs::entity_scene::EEntitySectionRequestError>
            loader_error;
    };

    template <typename T>
    using SectionResidencyExp = lux::cxx::expected<T, SectionResidencyFailure>;
}
