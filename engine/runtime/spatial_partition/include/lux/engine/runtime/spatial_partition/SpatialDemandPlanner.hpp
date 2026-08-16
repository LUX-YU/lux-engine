#pragma once
/**
 * @file SpatialDemandPlanner.hpp
 * @brief Transactional, dimension-neutral residency union planner.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/runtime/spatial_partition/EntitySectionRecordStore.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialDemand.hpp>
#include <lux/engine/runtime/spatial_partition/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace lux::runtime::spatial_partition
{
    struct SpatialResidentDemand final
    {
        lux::entity_scene::EntitySectionRecord record;
        std::size_t source_references{0u};
        std::uint32_t priority{0u};
    };

    struct SpatialDemandPlannerSnapshot final
    {
        std::uint64_t revision{0u};
        std::size_t source_count{0u};
        std::size_t dynamic_records{0u};
        std::size_t resident_sections{0u};
        std::size_t source_references{0u};
        std::uint64_t decoded_bytes{0u};
        std::uint64_t entity_count{0u};
        std::uint64_t maximum_decoded_bytes{0u};
        std::uint64_t maximum_entities{0u};
    };

    class SpatialDemandPlanner;

    class LUX_ENGINE_RUNTIME_SPATIAL_PARTITION_PUBLIC
    SpatialDemandPlan final
    {
    public:
        SpatialDemandPlan() = default;
        SpatialDemandPlan(SpatialDemandPlan&&) noexcept = default;
        SpatialDemandPlan& operator=(SpatialDemandPlan&&) noexcept = default;
        SpatialDemandPlan(const SpatialDemandPlan&) = delete;
        SpatialDemandPlan& operator=(const SpatialDemandPlan&) = delete;

        [[nodiscard]] std::span<const SpatialResidentDemand> residents()
            const noexcept
        {
            return residents_;
        }
        [[nodiscard]] SpatialDemandPlannerSnapshot snapshot() const noexcept
        {
            return snapshot_;
        }

    private:
        friend class SpatialDemandPlanner;

        struct SourceState final
        {
            SpatialDemandSourceId id;
            std::uint64_t generation{0u};
            lux::entity_scene::DemandChannelId channel;
            std::vector<SpatialDemandEntry> demands;
            std::vector<lux::entity_scene::EntitySectionRecord> records;
        };

        std::uint64_t base_revision_{0u};
        std::vector<SourceState> sources_;
        std::vector<SpatialResidentDemand> residents_;
        SpatialDemandPlannerSnapshot snapshot_;
    };

    class LUX_ENGINE_RUNTIME_SPATIAL_PARTITION_PUBLIC
    SpatialDemandPlanner final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            SpatialDemandPlanner,
            SpatialPartitionFailure>
        create(
            EntitySectionRecordStore records,
            SpatialPartitionBudget budget);

        SpatialDemandPlanner(SpatialDemandPlanner&&) noexcept = default;
        SpatialDemandPlanner& operator=(SpatialDemandPlanner&&) noexcept =
            default;
        SpatialDemandPlanner(const SpatialDemandPlanner&) = delete;
        SpatialDemandPlanner& operator=(const SpatialDemandPlanner&) =
            delete;

        [[nodiscard]] lux::cxx::expected<
            SpatialDemandPlan,
            SpatialPartitionFailure>
        prepareReplace(SpatialDemandSourceUpdate update) const;

        [[nodiscard]] lux::cxx::expected<
            SpatialDemandPlan,
            SpatialPartitionFailure>
        prepareRemove(
            const SpatialDemandSourceId& source,
            std::uint64_t generation) const;

        [[nodiscard]] lux::cxx::expected<void, SpatialPartitionFailure>
        commit(SpatialDemandPlan plan) noexcept;

        [[nodiscard]] std::span<const SpatialResidentDemand> residents()
            const noexcept
        {
            return residents_;
        }
        [[nodiscard]] SpatialDemandPlannerSnapshot snapshot() const noexcept
        {
            return snapshot_;
        }

    private:
        SpatialDemandPlanner(
            EntitySectionRecordStore records,
            SpatialPartitionBudget budget) noexcept
            : records_(std::move(records)), budget_(budget)
        {
            snapshot_.maximum_decoded_bytes = budget.maximum_decoded_bytes;
            snapshot_.maximum_entities = budget.maximum_entities;
        }

        [[nodiscard]] lux::cxx::expected<void, SpatialPartitionFailure>
        rebuild(SpatialDemandPlan& plan) const;

        EntitySectionRecordStore records_;
        SpatialPartitionBudget budget_;
        std::uint64_t revision_{0u};
        std::vector<SpatialDemandPlan::SourceState> sources_;
        std::vector<SpatialResidentDemand> residents_;
        SpatialDemandPlannerSnapshot snapshot_;
    };
}
