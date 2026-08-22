#pragma once
/**
 * @file SpatialPartitionSystem.hpp
 * @brief Schedule owner for dimension-neutral EntitySection demand.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/entity_scene/EntitySectionLoaderSystem.hpp>
#include <lux/engine/runtime/spatial_partition/SpatialDemandPlanner.hpp>
#include <lux/engine/runtime/spatial_partition/visibility.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace lux::runtime::spatial_partition
{
    struct SpatialPartitionSnapshot final
    {
        SpatialDemandPlannerSnapshot demand;
        std::size_t loader_tickets{0u};
        std::size_t waiting_sections{0u};
        std::size_t staging_sections{0u};
        std::size_t active_sections{0u};
        std::size_t failed_sections{0u};
        bool loader_binding_valid{false};
        std::uint64_t committed_replacements{0u};
        std::uint64_t committed_removals{0u};
        std::uint64_t rejected_transactions{0u};
        std::uint64_t budget_rejections{0u};
        std::uint64_t stale_generation_rejections{0u};
    };

    class LUX_ENGINE_RUNTIME_SPATIAL_PARTITION_PUBLIC
    SpatialPartitionSystem final : public lux::ecs::ISystem
    {
    public:
        SpatialPartitionSystem(
            lux::ecs::entity_scene::EntitySectionClient client,
            SpatialDemandPlanner planner) noexcept;
        ~SpatialPartitionSystem() override;

        SpatialPartitionSystem(const SpatialPartitionSystem&) = delete;
        SpatialPartitionSystem& operator=(const SpatialPartitionSystem&) =
            delete;

        [[nodiscard]] SpatialPartitionExp<void>
        replaceDemandSource(SpatialDemandSourceUpdate update);

        [[nodiscard]] SpatialPartitionExp<void>
        removeDemandSource(
            const SpatialDemandSourceId& source,
            std::uint64_t generation);

        [[nodiscard]] SpatialPartitionSnapshot snapshot() const noexcept;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        void update(const lux::ecs::SystemUpdateContext& context) override;
        [[nodiscard]] std::span<const Type> prerequisites() const noexcept
            override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept
            override;

    private:
        struct ResidentTicket;

        [[nodiscard]] SpatialPartitionExp<void>
        apply(SpatialDemandPlan plan, bool removal);
        void recordFailure(ESpatialPartitionError error) noexcept;
        void requireOwnerThread() const noexcept;

        lux::ecs::entity_scene::EntitySectionClient client_;
        SpatialDemandPlanner planner_;
        std::vector<ResidentTicket> resident_tickets_;
        std::thread::id owner_thread_;
        bool added_{false};
        bool binding_mismatch_{false};
        std::uint64_t committed_replacements_{0u};
        std::uint64_t committed_removals_{0u};
        std::uint64_t rejected_transactions_{0u};
        std::uint64_t budget_rejections_{0u};
        std::uint64_t stale_generation_rejections_{0u};
    };
}
