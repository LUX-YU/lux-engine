#pragma once

#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/graph/StageScope.hpp>

namespace lux::render::detail
{

    enum class EMemoryPlacement : std::uint8_t
    {
        EXCLUDED,
        DEDICATED,
        RENDERING_SCOPE,
        DISTINCT_REQUIREMENTS,
        INCOMPATIBLE_LAYOUT,
        UNORDERED,
        ALIASED
    };

    struct AllocationRootSlot final
    {
        std::uint32_t index;
    };

    struct MemoryGroupId final
    {
        std::uint32_t index;
    };

    struct RootMemoryPlacement final
    {
        AllocationRootSlot root;
        EMemoryPlacement reason;
    };

    struct BoundMemoryPlacement final
    {
        MemoryGroupId group;
        EMemoryPlacement reason;
    };

    [[nodiscard]] inline nri::GlobalBarrierDesc aliasHandoff(
        const AliasUseFrontier& previous, const AliasUseFrontier& next, std::size_t queue
    ) noexcept
    {
        const auto& local = previous.queues[queue];
        const auto& entry = next.queues[queue];
        nri::GlobalBarrierDesc barrier{local.source, entry.destination};
        bool has_foreign_source = false;
        for (std::size_t q = 0; q < previous.queues.size(); ++q)
        {
            const auto& source = previous.queues[q];
            if (q == queue || !source.point_count) continue;
            has_foreign_source = true;
            const auto waited = entry.waited_through[q];
            // Entry access alone does not describe the first image layout transition.
            const bool needs_full_scope = next.image || waited == kGraphInvalidIndex ||
                waited < source.last_submission;
            if (needs_full_scope)
            {
                barrier.before.stages = barrier.after.stages = nri::StageBits::ALL;
                barrier.after.access = entry.source.access;
                return barrier;
            }
        }
        if (has_foreign_source)
        {
            // Chain the semaphore wait to this barrier in a destination-queue stage.
            // Foreign accesses are made available by the source queue's whole-submit signal.
            barrier.before.stages = local.point_count ? combineStages(local.source.stages, entry.destination.stages)
                                                      : entry.destination.stages;
        }
        return barrier;
    }

    // Select only handoffs in the actual allocation groups. Cross-queue ordering is established by the submit DAG.
    [[nodiscard]] std::vector<std::optional<nri::GlobalBarrierDesc>> instantiateAliasBarriers(
        const PhysicalResourcePlan& physical, std::span<const BoundMemoryPlacement> placements, std::size_t pass_count
    ) noexcept;

    // Resize transfers the connected closure of old/new allocation groups, including fixed-size members.
    void expandMemoryMigration(
        std::span<const BoundMemoryPlacement> previous,
        std::span<const RootMemoryPlacement> next,
        std::span<std::uint8_t> changed
    ) noexcept;

    // Resolve actual requirements at Attach/Resize; each group is identified by its root physical slot.
    [[nodiscard]] std::vector<RootMemoryPlacement> instantiatePlacements(
        const PhysicalResourcePlan& physical,
        std::span<const nri::MemoryDesc> requirements,
        nri::GraphicsAPI backend
    ) noexcept;
} // namespace lux::render::detail
