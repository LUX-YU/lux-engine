#include <lux/engine/render/device/SubmissionQueue.hpp>
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

    namespace
    {
        // Overlapping command sequences and condition regions must stay in one queue and one batch.
        std::size_t recordingIntervalEnd(const RecordingProgram& recording, std::size_t first) noexcept
        {
            auto end = first + 1;
            for (auto p = first; p < end; ++p)
            {
                const auto& pass = recording.passes[p];
                const auto& unit = recording.units[pass.unit];
                end = std::max(end, std::size_t{unit.first_pass} + unit.pass_count);
                if (pass.condition_region == kGraphInvalidIndex) continue;
                const auto& region = recording.conditions[pass.condition_region];
                end = std::max(end, std::size_t{region.first_pass} + region.pass_count);
            }
            return end;
        }
    } // namespace

    bool
    RenderGraphCompiler::Workspace::assignAsyncCompute(
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
            const auto end = recordingIntervalEnd(recording, first);
            const bool can_migrate = std::ranges::all_of(order.subspan(first, end - first), [&](auto id)
            {
                return eligible(graph.passes()[id.index]);
            });
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

    namespace
    {
        // A wait discovered late in a queue run belongs at its consumer. Combining it with
        // the preceding independent work would move that wait to the start of the entire CB.
        using QueuePositions = std::array<std::uint32_t, 3>; // Position + 1; zero means no predecessor.

        std::vector<QueuePositions> foreignPredecessors(
            const RecordingProgram& recording, const AnalyzedGraph& source, const std::array<std::size_t, 3>& physical
        ) noexcept
        {
            const auto& passes = recording.passes;
            const auto queue = [&](std::size_t p) { return physical[static_cast<std::size_t>(passes[p].queue)]; };
            if (passes.empty()) return {};
            const auto first_queue = queue(0);
            const bool one_queue = std::ranges::all_of(passes, [&](const auto& pass)
            {
                return physical[static_cast<std::size_t>(pass.queue)] == first_queue;
            });
            if (one_queue) return {};
            std::vector<QueuePositions> required(passes.size());
            std::vector<std::uint32_t> head(passes.size());
            std::vector<std::uint32_t> positions(source.graph.passes().size(), kGraphInvalidIndex);
            for (std::size_t first = 0; first < passes.size();)
            {
                const auto end = recordingIntervalEnd(recording, first);
                for (auto p = first; p < end; ++p)
                {
                    head[p] = static_cast<std::uint32_t>(first);
                    positions[source.analysis.execution_order[p].index] = static_cast<std::uint32_t>(p);
                }
                first = end;
            }
            const auto require = [&](std::uint32_t producer, std::uint32_t consumer)
            {
                if (producer == kGraphInvalidIndex || consumer == kGraphInvalidIndex || queue(producer) == queue(consumer))
                    return;
                auto& position = required[head[consumer]][queue(producer)];
                position = std::max(position, producer + 1);
            };
            for (const auto& edge : source.analysis.dependencies)
                require(positions[edge.before.index], positions[edge.after.index]);
            for (std::uint32_t p = 0; p < passes.size(); ++p)
            {
                for (const auto& image : recording.imageBarriers(passes[p])) require(image.previous_slot, p);
                for (const auto& buffer : recording.bufferBarriers(passes[p])) require(buffer.previous_slot, p);
            }
            return required;
        }

        // Sort by (batch, operand) so repeated pass and boundary uses occupy one slot per batch.
        using BatchOperand = std::pair<std::uint32_t, std::uint32_t>;

        template<class Slot>
        void publishBatchOperands(
            std::vector<BatchOperand>& uses,
            std::vector<Slot>& slots,
            std::span<SubmissionBatch> batches,
            std::span<const Slot> SubmissionBatch::* member
        ) noexcept
        {
            if (!std::ranges::is_sorted(uses)) std::ranges::sort(uses);
            uses.erase(std::ranges::unique(uses).begin(), uses.end());
            slots.clear();
            slots.reserve(uses.size());
            auto use = uses.begin();
            for (std::size_t batch = 0; batch < batches.size(); ++batch)
            {
                const auto first = slots.size();
                while (use != uses.end() && use->first == batch)
                {
                    slots.push_back(Slot{use->second});
                    ++use;
                }
                batches[batch].*member = std::span{slots}.subspan(first);
            }
        }
    } // namespace

    void RenderGraphCompiler::Workspace::planSubmissions(compiled::RenderGraph& plan) noexcept
    {
        const auto& graph = source.graph;
        const auto& analysis = source.analysis;
        const auto& resources = plan.resources_;
        auto& recording = plan.recording_;
        const auto& queues = device.queues;
        auto& batches = plan.submissions_;
        const auto physical = physicalQueueIndices(queues);
        plan.last_batches_ = {};
        plan.writes_output_unconditionally_ = false;
        std::vector<BatchOperand> output_writes;
        const auto output = plan.output_;
        const bool presents = presentsOutput();
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
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        std::vector<BatchOperand> raster_lists;
#endif
        const auto required = foreignPredecessors(recording, source, physical);
        std::array<QueuePositions, 3> covered{};
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            auto& pass = recording.passes[p];
            bool new_wait = false;
            if (!required.empty())
            {
                auto& waits = covered[physical[static_cast<std::size_t>(pass.queue)]];
                for (std::size_t q = 0; q < waits.size(); ++q)
                {
                    if (!required[p][q]) continue;
                    const auto producer = recording.passes[required[p][q] - 1].submission + 1;
                    new_wait |= producer > waits[q];
                    waits[q] = std::max(waits[q], producer);
                }
            }
            if (batches.empty() || batches.back().queue != pass.queue || (new_wait && batches.back().pass_count))
            {
                batches.push_back({pass.queue, p});
            }
            const auto batch = static_cast<std::uint32_t>(batches.size() - 1);
            recording.passes[p].submission = batch;
            ++batches.back().pass_count;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
            if (const auto* raster = recording.rasterCommands(pass))
            {
                if (std::holds_alternative<RasterListCommands>(raster->draw)) raster_lists.emplace_back(batch, p);
            }
#endif
            pass_batches[analysis.execution_order[p].index] = batch;
            bool writes_output = false;
            for (const auto& use : graph.passes()[analysis.execution_order[p].index].uses)
            {
                const bool write = use.access != EGraphAccess::READ;
                writes_output |= write && use.version.resource.index == output;
                const bool uses_history = resources[use.version.resource.index].history != kGraphInvalidIndex ||
                    (!write && use.fallback && resources[use.fallback->resource.index].history != kGraphInvalidIndex);
                batches.back().uses_history |= uses_history;
            }
            if (!writes_output) continue;
            auto& submission = batches.back();
            if (submission.writes_output) continue;
            if (pass.condition_region == kGraphInvalidIndex)
            {
                submission.writes_output = true;
                plan.writes_output_unconditionally_ = true;
                while (!output_writes.empty() && output_writes.back().first == batch) output_writes.pop_back();
            }
            else
            {
                const BatchOperand write{batch, pass.condition_region};
                if (output_writes.empty() || output_writes.back() != write) output_writes.push_back(write);
            }
        }
        const bool has_restore = !recording.after.images.empty() || !recording.after.buffers.empty();
        // Native restoration and presentation have a Graphics epilogue when the last work uses another queue.
        if ((has_restore || presents) && batches.back().queue != EQueueRole::GRAPHICS)
        {
            batches.push_back({EQueueRole::GRAPHICS, static_cast<std::uint32_t>(recording.passes.size())});
        }
        // Physical queue FIFO covers every earlier producer on the same queue. Keep its latest
        // batch and accumulate all destination stages while each dependency is discovered.
        std::array<SubmissionDependency, 3> empty_waits{};
        for (auto& wait : empty_waits) wait.producer = kGraphInvalidIndex;
        std::vector waits(batches.size(), empty_waits);
        const auto depend = [&](std::uint32_t consumer, std::uint32_t producer, nri::StageBits stages) noexcept
        {
            if (consumer == producer) return;
            const auto queue = physical[static_cast<std::size_t>(batches[producer].queue)];
            auto& wait = waits[consumer][queue];
            if (wait.producer == kGraphInvalidIndex) wait = {producer, stages};
            else
            {
                wait.producer = std::max(wait.producer, producer);
                wait.stages = combineStages(wait.stages, stages);
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
                batches[pass.submission].uses_history |= resources[barrier.resource].history != kGraphInvalidIndex;
                if (barrier.previous_slot != kGraphInvalidIndex)
                {
                    const auto producer = recording.passes[barrier.previous_slot].submission;
                    depend(pass.submission, producer, barrier.native.after.stages);
                }
            };
            for (const auto& barrier : recording.imageBarriers(pass))
            {
                transitionDependency(barrier);
            }
            for (const auto& barrier : recording.bufferBarriers(pass))
            {
                transitionDependency(barrier);
            }
        }
        const auto last = static_cast<std::uint32_t>(batches.size() - 1);
        std::vector<bool> restored(resources.size());
        const auto finalHistory = [&](const auto& transition)
        {
            restored[transition.resource] = true;
            batches[last].uses_history |= resources[transition.resource].history != kGraphInvalidIndex;
        };
        for (const auto& transition : recording.after.images)
        {
            finalHistory(transition);
        }
        for (const auto& transition : recording.after.buffers)
        {
            finalHistory(transition);
        }
        publishBatchOperands(output_writes, plan.output_write_regions_, batches, &SubmissionBatch::output_regions);
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        publishBatchOperands(raster_lists, plan.batch_raster_list_passes_, batches, &SubmissionBatch::raster_list_passes);
#endif
        for (std::uint32_t b = 0; b < batches.size(); ++b)
        {
            const auto& batch = batches[b];
            if (batch.uses_history)
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
            auto& tail = plan.last_batches_.batches[q];
            if (tail != kGraphInvalidIndex) depend(b, tail, nri::StageBits::ALL);
            tail = b;
        }

        std::vector<std::size_t> offsets(batches.size() + 1);
        for (const auto& batch_waits : waits)
            for (const auto& edge : batch_waits)
                if (edge.producer != kGraphInvalidIndex) ++offsets[edge.producer + 1];
        for (std::size_t i = 1; i < offsets.size(); ++i) offsets[i] += offsets[i - 1];

        plan.batch_dependencies_.resize(offsets.back());
        plan.batch_successors_.resize(offsets.back());
        const auto dependencies = std::span{plan.batch_dependencies_};
        const auto successors = std::span{plan.batch_successors_};
        std::size_t end = 0;
        for (std::uint32_t i = 0; i < batches.size(); ++i)
        {
            const auto first = end;
            for (const auto& wait : waits[i])
                if (wait.producer != kGraphInvalidIndex) dependencies[end++] = wait;
            batches[i].dependencies = dependencies.subspan(first, end - first);
            batches[i].successors = successors.subspan(offsets[i], offsets[i + 1] - offsets[i]);
        }
        for (std::uint32_t consumer = 0; consumer < batches.size(); ++consumer)
            for (const auto& edge : batches[consumer].dependencies)
                successors[offsets[edge.producer]++] = {consumer, edge.stages};
    }
} // namespace lux::render::detail
