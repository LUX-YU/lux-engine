#include <lux/engine/render/graph/compile/GraphCompileWorkspace.hpp>
#include <lux/engine/render/graph/compile/GraphAnalysis.hpp>
#include <algorithm>
#include <lux/engine/render/graph/compile/SubmissionPlan.hpp>
#include <lux/engine/render/graph/compile/BarrierPlan.hpp>
#include <unordered_map>

namespace lux::render::detail
{
    std::array<std::size_t, 3> physicalQueueIndices(const std::array<const QueueProperties*, 3>& queues) noexcept
    {
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
        return physical_queue;
    }

    bool
    GraphCompiler::Workspace::assignAsyncCompute(
        compiled::RenderGraph& plan, std::uint32_t surface_output
    ) const noexcept
    {
        auto& recording = plan.recording_;
        const auto& graph = source.graph;
        const auto order = std::span(source.analysis.execution_order);
        const auto& queues = device.queues;
        if (queues[0] == queues[1])
        {
            return false;
        }
        const auto pinnedResource = [&](GraphResourceId id) noexcept
        {
            const auto& resource = graph.resources()[id.index];
            const bool imported_image = resource.origin == EGraphOrigin::IMPORTED &&
                std::holds_alternative<GraphImageDesc>(resource.description);
            const bool restricted = imported_image && !plan.resources_[id.index].concurrent_image_read;
            return restricted || id.index == surface_output;
        };
        const auto eligible = [&](const GraphPass& pass) noexcept
        {
            const auto* compute = std::get_if<ComputeWork>(&pass.command);
            if (!compute || !compute->allow_async_compute)
            {
                return false;
            }
            return std::ranges::none_of(pass.uses, [&](const auto& use) noexcept
            {
                return pinnedResource(use.version.resource) ||
                    (use.access == EGraphAccess::READ && use.fallback && pinnedResource(use.fallback->resource));
            });
        };
        bool assigned = false;
        for (std::size_t first = 0; first < order.size();)
        {
            // Sequences and conditional regions both require a single physical queue.
            // Close the interval over all overlapping units before assigning it.
            auto end = first + 1;
            bool can_migrate = true;
            for (auto slot = first; slot < end; ++slot)
            {
                const auto& pass = recording.passes[slot];
                const auto& unit = recording.units[pass.unit];
                end = std::max(end, std::size_t(unit.first_pass) + unit.pass_count);
                if (pass.condition_region != kGraphInvalidIndex)
                {
                    const auto& region = recording.conditions[pass.condition_region];
                    end = std::max(end, std::size_t(region.first_pass) + region.pass_count);
                }
                can_migrate = can_migrate && eligible(graph.passes()[order[slot].index]);
            }
            if (can_migrate)
            {
                for (auto& pass : std::span(recording.passes).subspan(first, end - first))
                {
                    pass.queue = EQueueRole::COMPUTE;
                }
                assigned = true;
            }
            first = end;
        }
        return assigned;
    }

    double estimateScheduleNs(
        const GraphDefinition& graph,
        std::span<const GraphPassId> order,
        std::span<const SubmissionBatch> batches,
        GraphQueueCosts costs
    ) noexcept
    {
        std::array<double, 2> queue_finish{};
        std::vector<double> batch_finish(batches.size());
        double submits = 0;
        for (std::size_t slot = 0; slot < batches.size(); ++slot)
        {
            const auto& batch = batches[slot];
            const bool is_compute = batch.queue == EQueueRole::COMPUTE;
            auto& available = queue_finish[is_compute];
            auto start = available;
            for (const auto& dependency : batch.dependencies)
            {
                if (batches[dependency.producer].queue != batch.queue)
                {
                    start = std::max(start, batch_finish[dependency.producer] + double(costs.queue_handoff_ns));
                }
            }
            double duration = 0;
            for (auto pass : order.subspan(batch.first_pass, batch.pass_count))
            {
                duration += double(graph.passes()[pass.index].estimated_gpu_ns);
            }
            available = batch_finish[slot] = start + duration;
            submits += double(is_compute ? costs.compute_submit_ns : costs.graphics_submit_ns);
        }
        return std::max(queue_finish[0], queue_finish[1]) + submits;
    }

    bool preservesObjectReuse(const PhysicalResourcePlan& baseline, const PhysicalResourcePlan& candidate) noexcept
    {
        for (std::size_t resource = 0; resource < baseline.resource_slots.size(); ++resource)
        {
            const auto baseline_slot = baseline.resource_slots[resource];
            if (!baseline_slot.valid())
            {
                continue;
            }
            const auto owner = baseline.slots[baseline_slot.index].resource.index;
            if (candidate.resource_slots[resource] != candidate.resource_slots[owner])
            {
                return false;
            }
        }
        return true;
    }

    void GraphCompiler::Workspace::planSubmissions(compiled::RenderGraph& plan) noexcept
    {
        const auto& graph = source.graph;
        const auto& analysis = source.analysis;
        const auto& resources = plan.resources_;
        auto& recording = plan.recording_;
        const auto& queues = device.queues;
        auto& batches = plan.submissions_;
        const auto physical = physicalQueueIndices(queues);
        plan.first_batches_ = {};
        plan.last_batches_ = {};
        plan.last_output_batches_ = {};
        plan.output_first_batch_ = kGraphInvalidIndex;
        auto& output_regions = plan.output_write_regions_;
        output_regions.clear();
        const auto output = plan.output_;
        const bool presents = plan.output_description_.kind == ETargetKind::SURFACE;
        pass_batches.assign(graph.passes().size(), kGraphInvalidIndex);
        // History initialization uses Graphics. Frame replica copies have their own upload frontier.
        const bool initializes_history = std::ranges::any_of(
            resources, [](const auto& resource) { return resource.history != kGraphInvalidIndex; }
        );
        if (initializes_history || recording.passes.empty())
        {
            // Keep the existing completion contract for a graph with no recording units.
            batches.push_back({});
        }
        const auto retain_use = [&](std::uint32_t batch, GraphResourceId resource)
        {
            if (resource.index == output)
            {
                const auto queue = physical[static_cast<std::size_t>(batches[batch].queue)];
                auto& tail = plan.last_output_batches_.batches[queue];
                if (tail == kGraphInvalidIndex || batch > tail) tail = batch;
                plan.output_first_batch_ = std::min(plan.output_first_batch_, batch);
            }
            if (resources[resource.index].origin == EGraphOrigin::IMPORTED)
                batches[batch].imported_resources.push_back(resource);
        };
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            auto& pass = recording.passes[p];
            if (batches.empty() || batches.back().queue != pass.queue)
            {
                batches.push_back({pass.queue, p});
                batches.back().first_output_region = static_cast<std::uint32_t>(output_regions.size());
            }
            const auto batch = static_cast<std::uint32_t>(batches.size() - 1);
            recording.passes[p].submission = batch;
            ++batches.back().pass_count;
            if (const auto* raster = recording.rasterCommands(pass))
            {
                if (const auto* stream = std::get_if<RasterListCommands>(&raster->draw))
                {
                    auto& reads = batches.back().shared_draw_reads;
                    if (std::ranges::find(reads, stream->data_index) == reads.end()) reads.push_back(stream->data_index);
                }
            }
            pass_batches[analysis.execution_order[p].index] = batch;
            bool writes_output = false;
            for (const auto& use : graph.passes()[analysis.execution_order[p].index].uses)
            {
                const auto add = [&](GraphResourceId resource)
                {
                    if (resources[resource.index].history != kGraphInvalidIndex)
                    {
                        batches.back().histories.push_back(resource);
                    }
                    retain_use(batch, resource);
                };
                const bool write = use.access != EGraphAccess::READ;
                writes_output |= write && use.version.resource.index == output;
                add(use.version.resource);
                if (use.access == EGraphAccess::READ && use.fallback)
                {
                    add(use.fallback->resource);
                }
            }
            if (!writes_output) continue;
            auto& submission = batches.back();
            if (pass.condition_region == kGraphInvalidIndex)
            {
                submission.writes_output = true;
            }
            else if (!submission.writes_output &&
                     (!submission.output_region_count || output_regions.back() != pass.condition_region))
            {
                output_regions.push_back(pass.condition_region);
                ++submission.output_region_count;
            }
        }
        const bool has_restore = !recording.after.images.empty() || !recording.after.buffers.empty();
        // Native restoration and presentation have a Graphics epilogue when the last work uses another queue.
        if ((has_restore || presents) && batches.back().queue != EQueueRole::GRAPHICS)
        {
            batches.push_back({EQueueRole::GRAPHICS, static_cast<std::uint32_t>(recording.passes.size())});
        }
        const auto depend = [&](std::uint32_t consumer, std::uint32_t producer, nri::StageBits stages)
        {
            if (consumer != producer)
            {
                batches[consumer].dependencies.push_back({producer, stages});
            }
        };
        // Consumer stages describe every declared use, even when a covered read emitted no barrier.
        const auto key = [](std::uint32_t a, std::uint32_t b) { return (std::uint64_t(a) << 32) | b; };
        std::unordered_map<std::uint64_t, nri::StageBits> dependency_stages;
        for (const auto& hazard : analysis.hazards)
        {
            const auto& uses = use_stages[hazard.after.index];
            const auto resource = std::ranges::lower_bound(uses, hazard.resource.index, {}, &ResourceUseStages::resource_index);
            const bool known = resource != uses.end() && resource->resource_index == hazard.resource.index;
            const auto stages = known ? resource->stages : nri::StageBits::ALL;
            auto [found, inserted] = dependency_stages.emplace(key(hazard.before.index, hazard.after.index), stages);
            if (!inserted)
            {
                found->second = combineStages(found->second, stages);
            }
        }
        for (const auto& edge : graph.dependencies())
        {
            // User-declared ordering need not be attributable to a resource.
            dependency_stages[key(edge.before.index, edge.after.index)] = nri::StageBits::ALL;
        }
        for (const auto& edge : analysis.dependencies)
        {
            const auto before = pass_batches[edge.before.index], after = pass_batches[edge.after.index];
            if (before != kGraphInvalidIndex && after != kGraphInvalidIndex)
            {
                const auto found = dependency_stages.find(key(edge.before.index, edge.after.index));
                depend(after, before, found == dependency_stages.end() ? nri::StageBits::ALL : found->second);
            }
        }
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            const auto& pass = recording.passes[p];
            const auto transitionDependency = [&](const auto& barrier)
            {
                if (barrier.previous_slot != kGraphInvalidIndex)
                {
                    const auto producer = recording.passes[barrier.previous_slot].submission;
                    depend(pass.submission, producer, barrier.native.after.stages);
                }
            };
            for (const auto& barrier : pass.before.images)
            {
                transitionDependency(barrier);
            }
            for (const auto& barrier : pass.before.buffers)
            {
                transitionDependency(barrier);
            }
        }
        const auto last = static_cast<std::uint32_t>(batches.size() - 1);
        std::vector<bool> restored(resources.size());
        const auto finalHistory = [&](const auto& transition)
        {
            restored[transition.resource] = true;
            if (resources[transition.resource].history != kGraphInvalidIndex)
            {
                batches[last].histories.push_back({transition.resource});
            }
        };
        for (const auto& transition : recording.after.images)
        {
            finalHistory(transition);
        }
        for (const auto& transition : recording.after.buffers)
        {
            finalHistory(transition);
        }
        for (std::uint32_t b = 0; b < batches.size(); ++b)
        {
            const auto& batch = batches[b];
            if (initializes_history && !batch.histories.empty())
            {
                depend(b, 0, nri::StageBits::ALL);
            }
            if (presents)
            {
                depend(last, b, nri::StageBits::ALL);
                continue;
            }
            if (!has_restore || b == last)
            {
                continue;
            }
            // A boundary transition waits for readers too, even when a covered read emitted no barrier.
            for (auto p = batch.first_pass; p < batch.first_pass + batch.pass_count; ++p)
            {
                const auto& uses = graph.passes()[analysis.execution_order[p].index].uses;
                const bool touches_restored = std::ranges::any_of(uses, [&](const auto& use)
                {
                    return restored[use.version.resource.index] ||
                           (use.access == EGraphAccess::READ && use.fallback && restored[use.fallback->resource.index]);
                });
                if (touches_restored)
                {
                    depend(last, b, nri::StageBits::ALL);
                    break;
                }
            }
        }
        // Object reuse and memory alias proofs include physical-Queue FIFO. Make it an explicit
        // edge before runtime is allowed to select ready batches out of recording-vector order.
        for (std::uint32_t b = 0; b < batches.size(); ++b)
        {
            const auto q = physical[static_cast<std::size_t>(batches[b].queue)];
            auto& head = plan.first_batches_.batches[q];
            head = std::min(head, b);
            auto& tail = plan.last_batches_.batches[q];
            if (tail != kGraphInvalidIndex) depend(b, tail, nri::StageBits::ALL);
            tail = b;
        }

        const auto retain_transitions = [&](std::uint32_t batch, const BarrierBatch& barriers)
        {
            for (const auto& transition : barriers.images) retain_use(batch, {transition.resource});
            for (const auto& transition : barriers.buffers) retain_use(batch, {transition.resource});
        };
        for (const auto& pass : recording.passes) retain_transitions(pass.submission, pass.before);
        retain_transitions(last, recording.after);
        for (auto& batch : batches)
        {
            auto& inputs = batch.imported_resources;
            std::ranges::sort(inputs, {}, &GraphResourceId::index);
            inputs.erase(std::ranges::unique(inputs).begin(), inputs.end());
            std::ranges::sort(batch.dependencies, {}, &SubmissionDependency::producer);
            std::size_t count = 0;
            for (const auto& dependency : batch.dependencies)
            {
                if (count != 0 && batch.dependencies[count - 1].producer == dependency.producer)
                {
                    auto& stages = batch.dependencies[count - 1].stages;
                    stages = combineStages(stages, dependency.stages);
                }
                else
                {
                    batch.dependencies[count++] = dependency;
                }
            }
            batch.dependencies.resize(count);
            // Each physical queue has explicit FIFO edges above and signals whole-submit
            // completion. Its latest producer covers earlier producers, with all wait stages.
            std::array<SubmissionDependency, 3> latest;
            for (auto& dependency : latest)
            {
                dependency.producer = kGraphInvalidIndex;
            }
            for (const auto& dependency : batch.dependencies)
            {
                const auto q = physical[static_cast<std::size_t>(batches[dependency.producer].queue)];
                auto& retained = latest[q];
                if (retained.producer == kGraphInvalidIndex)
                {
                    retained = dependency;
                }
                else
                {
                    retained.producer = std::max(retained.producer, dependency.producer);
                    retained.stages = combineStages(retained.stages, dependency.stages);
                }
            }
            batch.dependencies.clear();
            for (const auto& dependency : latest)
            {
                if (dependency.producer != kGraphInvalidIndex)
                {
                    batch.dependencies.push_back(dependency);
                }
            }
            std::ranges::sort(batch.dependencies, {}, &SubmissionDependency::producer);
            std::ranges::sort(batch.histories, {}, &GraphResourceId::index);
            batch.histories.erase(std::unique(batch.histories.begin(), batch.histories.end()), batch.histories.end());
        }
        for (std::uint32_t consumer = 0; consumer < batches.size(); ++consumer)
        {
            for (const auto& dependency : batches[consumer].dependencies)
            {
                batches[dependency.producer].successors.push_back({consumer, dependency.stages});
            }
        }
    }
} // namespace lux::render::detail
