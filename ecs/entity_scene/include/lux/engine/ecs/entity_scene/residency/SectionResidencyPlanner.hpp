#pragma once
/**
 * @file SectionResidencyPlanner.hpp
 * @brief Transactional, dimension-neutral residency union planner.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/entity_scene/residency/EntitySectionRecordStore.hpp>
#include <lux/engine/ecs/entity_scene/residency/SectionResidencyDemand.hpp>
#include <lux/engine/ecs/entity_scene/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace lux::ecs::entity_scene::residency
{
    struct SectionResidentDemand final
    {
        lux::ecs::scene_format::SectionRecord record;
        std::size_t source_references{0u};
        std::uint32_t priority{0u};
    };

    struct SectionResidencyPlannerSnapshot final
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

    class SectionResidencyPlanner;

    class LUX_ENGINE_ECS_ENTITY_SCENE_PUBLIC
    SectionResidencyPlan final
    {
    public:
        SectionResidencyPlan() = default;
        SectionResidencyPlan(SectionResidencyPlan&&) noexcept = default;
        SectionResidencyPlan& operator=(SectionResidencyPlan&&) noexcept = default;
        SectionResidencyPlan(const SectionResidencyPlan&) = delete;
        SectionResidencyPlan& operator=(const SectionResidencyPlan&) = delete;

        [[nodiscard]] std::span<const SectionResidentDemand> residents()
            const noexcept
        {
            return residents_;
        }
        [[nodiscard]] SectionResidencyPlannerSnapshot snapshot() const noexcept
        {
            return snapshot_;
        }

    private:
        friend class SectionResidencyPlanner;

        struct SourceState final
        {
            SectionDemandSourceId id;
            std::uint64_t generation{0u};
            lux::ecs::scene_format::DemandChannelId channel;
            std::vector<SectionDemandEntry> demands;
            std::vector<lux::ecs::scene_format::SectionRecord> records;
        };

        std::uint64_t base_revision_{0u};
        std::vector<SourceState> sources_;
        std::vector<SectionResidentDemand> residents_;
        SectionResidencyPlannerSnapshot snapshot_;
    };

    class LUX_ENGINE_ECS_ENTITY_SCENE_PUBLIC
    SectionResidencyPlanner final
    {
    public:
        [[nodiscard]] static SectionResidencyExp<SectionResidencyPlanner>
        create(
            EntitySectionRecordStore records,
            SectionResidencyBudget budget);

        SectionResidencyPlanner(SectionResidencyPlanner&&) noexcept = default;
        SectionResidencyPlanner& operator=(SectionResidencyPlanner&&) noexcept =
            default;
        SectionResidencyPlanner(const SectionResidencyPlanner&) = delete;
        SectionResidencyPlanner& operator=(const SectionResidencyPlanner&) =
            delete;

        [[nodiscard]] SectionResidencyExp<SectionResidencyPlan>
        prepareReplace(SectionDemandSourceUpdate update) const;

        [[nodiscard]] SectionResidencyExp<SectionResidencyPlan>
        prepareRemove(
            const SectionDemandSourceId& source,
            std::uint64_t generation) const;

        [[nodiscard]] SectionResidencyExp<void>
        commit(SectionResidencyPlan plan) noexcept;

        [[nodiscard]] std::span<const SectionResidentDemand> residents()
            const noexcept
        {
            return residents_;
        }
        [[nodiscard]] SectionResidencyPlannerSnapshot snapshot() const noexcept
        {
            return snapshot_;
        }

    private:
        SectionResidencyPlanner(
            EntitySectionRecordStore records,
            SectionResidencyBudget budget) noexcept
            : records_(std::move(records)), budget_(budget)
        {
            snapshot_.maximum_decoded_bytes = budget.maximum_decoded_bytes;
            snapshot_.maximum_entities = budget.maximum_entities;
        }

        [[nodiscard]] SectionResidencyExp<void>
        rebuild(SectionResidencyPlan& plan) const;

        EntitySectionRecordStore records_;
        SectionResidencyBudget budget_;
        std::uint64_t revision_{0u};
        std::vector<SectionResidencyPlan::SourceState> sources_;
        std::vector<SectionResidentDemand> residents_;
        SectionResidencyPlannerSnapshot snapshot_;
    };
}
