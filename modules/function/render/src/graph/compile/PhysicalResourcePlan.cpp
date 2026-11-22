#include <lux/engine/render/graph/compile/GraphCompileWorkspace.hpp>
#include <lux/engine/render/extensions/graph/RenderGraph.hpp>
#include <algorithm>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/graph/compile/SubmissionPlan.hpp>
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

    } // namespace

    PhysicalResourcePlan GraphCompiler::Workspace::planPhysicalResources(const compiled::RenderGraph& plan) noexcept
    {
        const auto& graph = source.graph;
        const auto source_order = std::span(source.analysis.execution_order);
        const auto& resources = plan.resources_;
        const auto& recording = plan.recording_;
        const auto& submissions = plan.submissions_;
        const auto& queues = device.queues;
        auto& reasons = diagnostics.resource_reuse;
        PhysicalResourcePlan result;
        result.allocation = allocation;
        result.resource_slots.resize(resources.size());
        reasons.assign(resources.size(), EResourceReuse::UNUSED);
        std::vector<ResourceInterval> intervals(resources.size());
        std::vector<Frontier> completed(submissions.size());
        std::array<Frontier, 3> queue_frontiers{};
        const auto physical_queue = physicalQueueIndices(queues);
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
                    interval.conditional |= pass.condition_region != kGraphInvalidIndex;
                    interval.end[q] = frontier[q];
                    for (std::size_t axis = 0; axis < before.size(); ++axis)
                    {
                        interval.before[axis] = std::min(interval.before[axis], before[axis]);
                    }
                };
                for (const auto& use : graph.passes()[source_order[p].index].uses)
                {
                    record(use.version.resource);
                    if (use.access == EGraphAccess::READ && use.fallback)
                    {
                        record(use.fallback->resource);
                    }
                }
            }
            completed[b] = queue_frontiers[q] = frontier;
        }
        std::vector<bool> exported(resources.size()), target(resources.size());
        for (const auto& output : graph.outputs())
        {
            exported[output.version.resource.index] = true;
            if (output.alternate) exported[output.alternate->value.resource.index] = true;
            target[output.version.resource.index] = target[output.version.resource.index] || output.target.has_value();
        }
        std::vector<std::uint32_t> order(resources.size());
        std::iota(order.begin(), order.end(), 0u);
        std::ranges::stable_sort(order, {}, [&](auto i) { return intervals[i].first_pass; });
        std::vector<std::uint32_t> tails;
        for (auto i : order)
        {
            const auto& resource = resources[i];
            auto& reason = reasons[i];
            if (!resource.memory_used)
            {
                continue;
            }
            if (resource.history != kGraphInvalidIndex)
            {
                reason = EResourceReuse::HISTORY;
                continue;
            }
            const bool runtime_bound =
                resource.origin == EGraphOrigin::IMPORTED || resource.origin == EGraphOrigin::FRAME_BUFFER;
            if (runtime_bound || resource.shared_table)
            {
                reason = EResourceReuse::EXTERNAL;
                continue;
            }
            if (const auto* image = std::get_if<GraphImageDesc>(&resource.description); image && image->runtime_layers)
            {
                reasons[i] = EResourceReuse::DYNAMIC_EXTENT;
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
            const bool alias_eligible = reason == EResourceReuse::DISTINCT_DESCRIPTION;
            if (alias_eligible)
            {
                for (std::uint32_t s = 0; s < tails.size(); ++s)
                {
                    const auto previous = tails[s];
                    const auto& candidate = resources[previous];
                    const bool reusable = result.slots[s].alias_eligible;
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
                        result.resource_slots[i] = {s};
                        tails[s] = i;
                        reason = EResourceReuse::REUSED;
                        break;
                    }
                }
            }
            if (!result.resource_slots[i].valid())
            {
                result.resource_slots[i] = {static_cast<std::uint32_t>(result.slots.size())};
                auto& slot = result.slots.emplace_back();
                slot.resource = {i};
                slot.before = intervals[i].before;
                slot.alias_eligible = alias_eligible;
                tails.push_back(i);
            }
            auto& slot = result.slots[result.resource_slots[i].index];
            for (std::size_t q = 0; q < queues.size(); ++q)
            {
                slot.before[q] = std::min(slot.before[q], intervals[i].before[q]);
                slot.end[q] = std::max(slot.end[q], intervals[i].end[q]);
            }
        }
        return result;
    }

    std::vector<RootMemoryPlacement> instantiatePlacements(
        const PhysicalResourcePlan& physical,
        std::span<const nri::MemoryDesc> requirements,
        nri::GraphicsAPI backend
    ) noexcept
    {
        std::vector<RootMemoryPlacement> placements(physical.slots.size());
        std::vector<std::uint32_t> tails;
        for (std::uint32_t i = 0; i < physical.slots.size(); ++i)
        {
            const auto& slot = physical.slots[i];
            auto& placement = placements[i];
            placement = {{i}, EMemoryPlacement::EXCLUDED};
            if (!slot.alias_eligible)
            {
                continue;
            }
            if (requirements[i].mustBeDedicated)
            {
                placement.reason = EMemoryPlacement::DEDICATED;
                continue;
            }
            const auto& frontier = physical.alias_frontiers[i];
            if (!frontier.rendering_boundary)
            {
                placement.reason = EMemoryPlacement::RENDERING_SCOPE;
                continue;
            }
            placement.reason = EMemoryPlacement::DISTINCT_REQUIREMENTS;
            const bool requires_compatible_layout = backend == nri::GraphicsAPI::D3D12 && frontier.image;
            for (auto& tail : tails)
            {
                const auto& previous = physical.slots[tail];
                const bool same_type = requirements[tail].type == requirements[i].type;
                // Keep linear buffers and non-linear textures in separate allocations. Every member
                // starts at offset zero: there is no adjacent mixed-granularity placement to pad.
                const auto& prior = physical.alias_frontiers[tail];
                const bool same_kind = prior.image == frontier.image;
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
                    // The placed-memory handoff currently uses a global barrier. On D3D12 the
                    // same access masks can share that boundary without a texture layout transition.
                    const auto access = [](const AliasQueueFrontier& q) { return q.source.access; };
                    const bool incompatible = requires_compatible_layout &&
                        !std::ranges::equal(prior.queues, frontier.queues, {}, access, access);
                    if (incompatible)
                    {
                        placement.reason = EMemoryPlacement::INCOMPATIBLE_LAYOUT;
                        continue;
                    }
                    placement = {placements[tail].root, EMemoryPlacement::ALIASED};
                    tail = i;
                    break;
                }
            }
            if (placement.root.index == i)
            {
                tails.push_back(i);
            }
        }
        return placements;
    }

    void expandMemoryMigration(
        std::span<const BoundMemoryPlacement> previous,
        std::span<const RootMemoryPlacement> next,
        std::span<std::uint8_t> changed
    ) noexcept
    {
        std::vector<std::uint32_t> parent(changed.size());
        std::iota(parent.begin(), parent.end(), 0);
        const auto root = [&](std::uint32_t index)
        {
            while (parent[index] != index)
            {
                parent[index] = parent[parent[index]];
                index = parent[index];
            }
            return index;
        };
        std::vector<std::uint32_t> previous_roots(changed.size(), kGraphInvalidIndex);
        for (std::uint32_t slot = 0; slot < changed.size(); ++slot)
        {
            auto& first = previous_roots[previous[slot].group.index];
            if (first == kGraphInvalidIndex)
            {
                first = slot;
            }
            // Mark a changed group even if its resource extent stayed fixed.
            if (first != next[slot].root.index) changed[slot] = true;
            parent[root(slot)] = root(first);
            parent[root(slot)] = root(next[slot].root.index);
        }
        std::vector<std::uint8_t> migrate(parent.size());
        for (std::uint32_t slot = 0; slot < changed.size(); ++slot)
        {
            migrate[root(slot)] |= changed[slot];
        }
        for (std::uint32_t slot = 0; slot < changed.size(); ++slot)
        {
            changed[slot] = migrate[root(slot)];
        }
    }

} // namespace lux::render::detail
