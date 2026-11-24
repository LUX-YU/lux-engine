#include <lux/engine/render/graph/compile/GraphCompileWorkspace.hpp>
#include <lux/engine/render/graph/compile/BarrierPlan.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/graph/compile/SubmissionPlan.hpp>
#include <algorithm>
#include <bit>
#include <utility>

namespace lux::render::detail
{
    namespace
    {
        template <class Matches>
        bool coversImageCells(const GraphImageRange& range, std::span<const ImageTransition> barriers, Matches matches)
        {
            // planBarriers combines uses per resource/cell and emits at most one transition before compaction.
            const auto cells = std::uint64_t(std::popcount(range.aspects)) * range.mip_count * range.layer_count;
            std::uint64_t covered = 0;
            for (const auto& transition : barriers)
            {
                const auto& cell = transition.native;
                const auto aspect = cell.planes == nri::PlaneBits::COLOR ? 1u :
                    cell.planes == nri::PlaneBits::DEPTH ? 2u : 4u;
                const bool selected = (range.aspects & aspect) && cell.mipOffset >= range.first_mip &&
                    cell.mipOffset - range.first_mip < range.mip_count && cell.layerOffset >= range.first_layer &&
                    cell.layerOffset - range.first_layer < range.layer_count;
                if (selected && matches(transition)) ++covered;
            }
            return cells != 0 && covered == cells;
        }

        bool coversLayoutEntry(
            GraphResourceId resource, const AliasUsePoint& point, std::span<const ImageTransition> barriers
        ) noexcept
        {
            return coversImageCells(std::get<GraphImageRange>(point.range), barriers, [&](const auto& transition)
            {
                const auto& native = transition.native;
                const bool discard = native.before.layout == nri::Layout::UNDEFINED &&
                    transition.previous_slot == kGraphInvalidIndex && native.before.access == nri::AccessBits::NONE;
                const bool follows_use = native.before.layout != nri::Layout::UNDEFINED &&
                    transition.previous_slot < point.first_pass;
                const bool on_path = transition.resource == resource.index && transition.boundary == ETransitionBoundary::PASS &&
                    native.after.layout != nri::Layout::UNDEFINED && (discard || follows_use);
                // The global barrier and this transition are separate native commands on the same queue.
                return on_path && coversStages(native.before.stages, point.scope.stages) &&
                    coversScope({native.after.access, native.after.stages}, point.scope);
            });
        }

        bool hasEntryDependency(
            const AliasResourceUse& use, const RecordingProgram& recording, std::span<const AliasUsePoint> earlier
        ) noexcept
        {
            const auto ordered = [&](const auto& transition)
            {
                const auto& barrier = transition.native;
                const bool is_local_use = transition.resource == use.resource.index &&
                    transition.previous_slot != kGraphInvalidIndex && transition.previous_slot < use.point.first_pass;
                if (!is_local_use || !coversScope({barrier.after.access, barrier.after.stages}, use.point.scope))
                    return false;
                return std::ranges::any_of(earlier, [&](const auto& prior)
                {
                    return prior.last_pass == transition.previous_slot && prior.range == use.point.range &&
                        coversScope({barrier.before.access, barrier.before.stages}, prior.scope);
                });
            };
            const auto* image = std::get_if<GraphImageRange>(&use.point.range);
            const auto& pass = recording.passes[use.point.first_pass];
            if (!image) return std::ranges::any_of(recording.bufferBarriers(pass), ordered);

            return coversImageCells(*image, recording.imageBarriers(pass), ordered);
        }
    } // namespace

    void RenderGraphCompiler::Workspace::publishAliasFrontiers(compiled::RenderGraph& plan) const noexcept
    {
        auto& physical = plan.physical_resources_;
        const auto& recording = plan.recording_;
        const auto physical_queue = physicalQueueIndices(device.queues);
        using QueuePoints = std::array<std::vector<AliasUsePoint>, 3>;
        std::vector<QueuePoints> points(physical.slots.size());
        for (const auto& use : alias_uses)
        {
            const auto slot = physical.resource_slots[use.resource.index];
            if (!slot.valid() || !physical.slots[slot.index].alias_eligible) continue;
            const auto role = recording.passes[use.point.first_pass].queue;
            auto& frontier = points[slot.index][physical_queue[static_cast<std::size_t>(role)]];
            // Same queue and native scope cover subsequent occurrences of this range. A different range,
            // write mode or stage retains its own point even when the two uses are in consecutive passes.
            const auto same = std::ranges::find_if(frontier, [&](const AliasUsePoint& p) noexcept
            {
                return p.range == use.point.range && p.scope.access == use.point.scope.access &&
                    p.scope.stages == use.point.scope.stages && p.writes == use.point.writes;
            });
            if (same != frontier.end())
            {
                same->last_pass = use.point.last_pass;
                continue;
            }
            auto point = use.point;
            point.needs_entry = !hasEntryDependency(use, recording, frontier);
            frontier.push_back(point);
        }

        physical.alias_frontiers.resize(points.size());
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            auto& slot = physical.alias_frontiers[i];
            const auto id = physical.slots[i].resource;
            const auto* image = std::get_if<GraphImageDesc>(&plan.resources_[id.index].description);
            slot.image = image != nullptr;
            for (std::size_t q = 0; q < slot.queues.size(); ++q)
            {
                const auto& selected = points[i][q];
                if (selected.empty()) continue;
                auto& queue = slot.queues[q];
                queue.first_point = static_cast<std::uint32_t>(physical.alias_points.size());
                queue.point_count = static_cast<std::uint32_t>(selected.size());
                queue.barrier_pass = selected.front().first_pass;
                queue.last_submission = recording.passes[selected.front().last_pass].submission;
                queue.source = queue.destination = selected.front().scope;
                for (const auto& point : std::span{selected}.subspan(1))
                {
                    queue.last_submission = std::max(queue.last_submission, recording.passes[point.last_pass].submission);
                    queue.source.access |= point.scope.access;
                    queue.source.stages = combineStages(queue.source.stages, point.scope.stages);
                    if (point.needs_entry)
                    {
                        queue.destination.access |= point.scope.access;
                        queue.destination.stages = combineStages(queue.destination.stages, point.scope.stages);
                    }
                }
                // Preserve D3D12's matching texture access contract at the global handoff.
                if (slot.image && device.limits.graphicsAPI == nri::GraphicsAPI::D3D12) queue.destination = queue.source;
                const auto& entry = recording.passes[queue.barrier_pass];
                const bool legacy_texture = image && device.limits.graphicsAPI == nri::GraphicsAPI::D3D12 &&
                    !device.limits.features.enhancedBarriers;
                const bool covers_layout = !image || (!legacy_texture && std::ranges::all_of(selected, [&](const auto& point)
                {
                    return !point.needs_entry || coversLayoutEntry(id, point, recording.imageBarriers(recording.passes[point.first_pass]));
                }));
                for (const auto& wait : plan.submissions_[entry.submission].dependencies)
                {
                    const auto role = plan.submissions_[wait.producer].queue;
                    const auto producer = physical_queue[static_cast<std::size_t>(role)];
                    const bool covers_entry = queue.destination.stages != nri::StageBits::NONE &&
                        coversStages(wait.stages, queue.destination.stages);
                    if (producer != q && covers_entry && covers_layout) queue.waited_through[producer] = wait.producer;
                }
                const auto group = entry.rendering_group;
                slot.rendering_boundary &= group == kGraphInvalidIndex ||
                    recording.groups[group].first_pass == queue.barrier_pass;
                physical.alias_points.insert(physical.alias_points.end(), selected.begin(), selected.end());
            }
        }
    }

    std::vector<std::optional<nri::GlobalBarrierDesc>> instantiateAliasBarriers(
        const PhysicalResourcePlan& physical, std::span<const BoundMemoryPlacement> placements, std::size_t pass_count
    ) noexcept
    {
        std::vector<std::optional<nri::GlobalBarrierDesc>> barriers(pass_count);
        std::vector<std::uint32_t> tails(placements.size(), kGraphInvalidIndex);
        for (std::uint32_t i = 0; i < placements.size(); ++i)
        {
            const auto previous = std::exchange(tails[placements[i].group.index], i);
            if (previous == kGraphInvalidIndex) continue;
            const auto& source = physical.alias_frontiers[previous];
            const auto& target = physical.alias_frontiers[i];
            for (std::size_t q = 0; q < target.queues.size(); ++q)
            {
                const auto& entry = target.queues[q];
                if (!entry.point_count) continue;
                const auto handoff = aliasHandoff(source, target, q);
                auto& barrier = barriers[entry.barrier_pass];
                if (!barrier)
                {
                    barrier = handoff;
                    continue;
                }
                barrier->before.access |= handoff.before.access;
                barrier->after.access |= handoff.after.access;
                barrier->before.stages = combineStages(barrier->before.stages, handoff.before.stages);
                barrier->after.stages = combineStages(barrier->after.stages, handoff.after.stages);
            }
        }
        return barriers;
    }
} // namespace lux::render::detail
