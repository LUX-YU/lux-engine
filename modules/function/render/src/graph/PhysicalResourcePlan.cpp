#include <algorithm>
#include <lux/engine/render/graph/PhysicalResourcePlan.hpp>
#include <numeric>

namespace lux::render::detail
{
    namespace
    {
        using Frontier = std::array<std::uint32_t, 3>;

        struct ResourceInterval final
        {
            Frontier before{kGraphInvalidIndex, kGraphInvalidIndex, kGraphInvalidIndex};
            Frontier end{};
            std::uint32_t first_pass = kGraphInvalidIndex;
            std::array<std::uint32_t, 3> first_queue_pass{kGraphInvalidIndex, kGraphInvalidIndex, kGraphInvalidIndex};
            std::array<nri::AccessBits, 3> accesses{};
            bool conditional = false;
        };

        void merge(Frontier& destination, const Frontier& source) noexcept
        {
            for (std::size_t q = 0; q < destination.size(); ++q)
            {
                destination[q] = std::max(destination[q], source[q]);
            }
        }

        bool precedes(const ResourceInterval& earlier, const ResourceInterval& later) noexcept
        {
            for (std::size_t q = 0; q < earlier.end.size(); ++q)
            {
                if (earlier.end[q] > later.before[q])
                {
                    return false;
                }
            }
            return true;
        }

        bool allowsReuse(EResourceReuse reason) noexcept
        {
            return reason == EResourceReuse::DISTINCT_DESCRIPTION || reason == EResourceReuse::UNORDERED ||
                   reason == EResourceReuse::REUSED;
        }
    } // namespace

    PhysicalResourcePlan planPhysicalResources(
        const RenderGraph& graph,
        std::span<const CompiledResource> resources,
        const RecordingProgram& recording,
        std::span<const SubmissionBatch> submissions,
        std::array<const QueueProperties*, 3> queues,
        ETransientAllocation allocation
    ) noexcept
    {
        PhysicalResourcePlan result;
        result.allocation = allocation;
        result.resource_slots.resize(resources.size(), kGraphInvalidIndex);
        result.reasons.resize(resources.size(), EResourceReuse::UNUSED);
        std::vector<ResourceInterval> intervals(resources.size());
        std::vector<Frontier> completed(submissions.size());
        std::array<Frontier, 3> queue_frontiers{};
        std::array<std::size_t, 3> physical_queue{};
        for (std::size_t q = 0; q < queues.size(); ++q)
        {
            physical_queue[q] = q;
            for (std::size_t previous = 0; previous < q; ++previous)
            {
                if (queues[q] == queues[previous])
                {
                    physical_queue[q] = physical_queue[previous];
                    break;
                }
            }
        }
        // A vector clock contains only existing batch dependencies and physical Queue FIFO.
        // The resource's end joins all uses; before meets the prerequisites of every use.
        for (std::size_t b = 0; b < submissions.size(); ++b)
        {
            const auto& batch = submissions[b];
            const auto q = physical_queue[static_cast<std::size_t>(batch.queue)];
            auto frontier = queue_frontiers[q];
            for (const auto& dependency : batch.dependencies)
            {
                merge(frontier, completed[dependency.producer]);
            }
            for (auto p = batch.first_pass; p < batch.first_pass + batch.pass_count; ++p)
            {
                const auto before = frontier;
                ++frontier[q];
                const auto& pass = recording.passes[p];
                const auto record = [&](GraphResourceId id)
                {
                    auto& interval = intervals[id.index];
                    interval.first_pass = std::min(interval.first_pass, p);
                    interval.first_queue_pass[q] = std::min(interval.first_queue_pass[q], p);
                    interval.conditional |= pass.condition_region != kGraphInvalidIndex;
                    interval.end[q] = frontier[q];
                    for (std::size_t axis = 0; axis < before.size(); ++axis)
                    {
                        interval.before[axis] = std::min(interval.before[axis], before[axis]);
                    }
                };
                for (const auto& use : graph.passes()[pass.source_pass].uses)
                {
                    record(use.version.resource);
                    if (use.access == EGraphAccess::READ && use.fallback)
                    {
                        record(use.fallback->resource);
                    }
                }
                for (const auto& transition : pass.before.images)
                {
                    intervals[transition.resource].accesses[q] |= transition.native.after.access;
                }
                for (const auto& transition : pass.before.buffers)
                {
                    intervals[transition.resource].accesses[q] |= transition.native.after.access;
                }
            }
            completed[b] = queue_frontiers[q] = frontier;
        }
        std::vector<bool> exported(resources.size()), target(resources.size());
        for (const auto& output : graph.outputs())
        {
            exported[output.version.resource.index] = true;
            target[output.version.resource.index] = target[output.version.resource.index] || output.target.has_value();
        }
        std::vector<std::uint32_t> order(resources.size());
        std::iota(order.begin(), order.end(), 0u);
        std::ranges::stable_sort(order, {}, [&](auto i) { return intervals[i].first_pass; });
        std::vector<std::uint32_t> tails;
        for (auto i : order)
        {
            const auto& resource = resources[i];
            auto& reason = result.reasons[i];
            if (!resource.memory_used)
            {
                continue;
            }
            if (resource.history != kGraphInvalidIndex)
            {
                reason = EResourceReuse::HISTORY;
                continue;
            }
            if (resource.origin == EGraphOrigin::IMPORTED || resource.shared_table)
            {
                reason = EResourceReuse::EXTERNAL;
                continue;
            }
            if (target[i])
            {
                reason = EResourceReuse::EXPORTED;
                continue;
            }
            reason = exported[i]                                       ? EResourceReuse::EXPORTED
                     : intervals[i].conditional                        ? EResourceReuse::CONDITIONAL
                     : !graph.resources()[i].initial_ranges.empty()    ? EResourceReuse::INITIAL_CONTENT
                     : allocation == ETransientAllocation::INDEPENDENT ? EResourceReuse::DISABLED
                                                                       : EResourceReuse::DISTINCT_DESCRIPTION;
            if (reason == EResourceReuse::DISTINCT_DESCRIPTION)
            {
                for (std::uint32_t s = 0; s < tails.size(); ++s)
                {
                    const auto previous = tails[s];
                    const auto& candidate = resources[previous];
                    const auto previous_reason = result.reasons[previous];
                    const bool reusable = allowsReuse(previous_reason);
                    // Legacy RGVulkanResourceAllocator::makeKey matches the complete description/usage.
                    // Reuse here additionally requires batch happens-before, never a frame-age heuristic.
                    const bool same_description = candidate.description == resource.description &&
                                                  candidate.image_usage == resource.image_usage &&
                                                  candidate.buffer_usage == resource.buffer_usage;
                    if (!reusable || !same_description)
                    {
                        continue;
                    }
                    reason = EResourceReuse::UNORDERED;
                    if (precedes(intervals[previous], intervals[i]))
                    {
                        result.resource_slots[i] = s;
                        tails[s] = i;
                        reason = EResourceReuse::REUSED;
                        break;
                    }
                }
            }
            if (result.resource_slots[i] == kGraphInvalidIndex)
            {
                result.resource_slots[i] = static_cast<std::uint32_t>(result.slots.size());
                result.slots.push_back({{i}, intervals[i].before});
                tails.push_back(i);
            }
            auto& slot = result.slots[result.resource_slots[i]];
            for (std::size_t q = 0; q < queues.size(); ++q)
            {
                slot.before[q] = std::min(slot.before[q], intervals[i].before[q]);
                slot.end[q] = std::max(slot.end[q], intervals[i].end[q]);
                slot.first_pass[q] = std::min(slot.first_pass[q], intervals[i].first_queue_pass[q]);
                slot.accesses[q] |= intervals[i].accesses[q];
            }
        }
        return result;
    }

    std::vector<MemoryPlacement> instantiatePlacements(
        const PhysicalResourcePlan& physical,
        std::span<const CompiledResource> resources,
        const RecordingProgram& recording,
        std::span<const nri::MemoryDesc> requirements
    ) noexcept
    {
        std::vector<MemoryPlacement> placements(physical.slots.size());
        std::vector<std::uint32_t> tails;
        for (std::uint32_t i = 0; i < physical.slots.size(); ++i)
        {
            const auto& slot = physical.slots[i];
            auto& placement = placements[i];
            placement = {i, EMemoryPlacement::EXCLUDED};
            if (!allowsReuse(physical.reasons[slot.resource.index]))
            {
                continue;
            }
            if (requirements[i].mustBeDedicated)
            {
                placement.reason = EMemoryPlacement::DEDICATED;
                continue;
            }
            const bool boundary = std::ranges::all_of(
                slot.first_pass,
                [&](auto p)
                {
                    if (p == kGraphInvalidIndex)
                    {
                        return true;
                    }
                    const auto group = recording.passes[p].rendering_group;
                    return group == kGraphInvalidIndex || recording.groups[group].first_pass == p;
                }
            );
            if (!boundary)
            {
                placement.reason = EMemoryPlacement::RENDERING_SCOPE;
                continue;
            }
            placement.reason = EMemoryPlacement::DISTINCT_REQUIREMENTS;
            for (auto& tail : tails)
            {
                const auto& previous = physical.slots[tail];
                const bool same_type = requirements[tail].type == requirements[i].type;
                // Keep linear buffers and non-linear textures in separate allocations. Every member
                // starts at offset zero: there is no adjacent mixed-granularity placement to pad.
                const bool same_kind = resources[previous.resource.index].description.index() ==
                                       resources[slot.resource.index].description.index();
                if (!same_type || !same_kind)
                {
                    continue;
                }
                placement.reason = EMemoryPlacement::UNORDERED;
                bool ordered = true;
                for (std::size_t q = 0; q < slot.before.size(); ++q)
                {
                    ordered &= previous.end[q] <= slot.before[q];
                }
                if (ordered)
                {
                    placement = {placements[tail].allocation, EMemoryPlacement::ALIASED};
                    tail = i;
                    break;
                }
            }
            if (placement.allocation == i)
            {
                tails.push_back(i);
            }
        }
        return placements;
    }
} // namespace lux::render::detail
