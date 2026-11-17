#include <algorithm>
#include <lux/engine/render/graph/Range.hpp>
#include <lux/engine/render/graph/compile/SubmissionPlan.hpp>
#include <tuple>
#include <unordered_map>

namespace lux::render::detail
{
    namespace
    {
        nri::StageBits combineStages(nri::StageBits a, nri::StageBits b) noexcept
        {
            return a == nri::StageBits::ALL || b == nri::StageBits::ALL ? nri::StageBits::ALL : a | b;
        }

        void compactImportedAccesses(std::vector<ImportedAccess>& accesses) noexcept
        {
            std::ranges::sort(
                accesses,
                [](const auto& a, const auto& b)
                {
                    if (a.batch != b.batch)
                    {
                        return a.batch < b.batch;
                    }
                    if (a.resource != b.resource)
                    {
                        return a.resource.index < b.resource.index;
                    }
                    // One resource has one range kind, already normalized by the Builder.
                    if (const auto* bytes = std::get_if<GraphBufferRange>(&a.range))
                    {
                        const auto& other = std::get<GraphBufferRange>(b.range);
                        return std::tie(bytes->offset, bytes->size) < std::tie(other.offset, other.size);
                    }
                    const auto imageKey = [](const GraphImageRange& range)
                    {
                        return std::tie(
                            range.aspects,
                            range.first_mip,
                            range.mip_count,
                            range.first_layer,
                            range.layer_count
                        );
                    };
                    return imageKey(std::get<GraphImageRange>(a.range)) < imageKey(std::get<GraphImageRange>(b.range));
                }
            );
            std::size_t count = 0;
            for (const auto& access : accesses)
            {
                const bool is_duplicate = count != 0 && accesses[count - 1].batch == access.batch &&
                                          accesses[count - 1].resource == access.resource &&
                                          accesses[count - 1].range == access.range;
                if (is_duplicate)
                {
                    accesses[count - 1].write |= access.write;
                    accesses[count - 1].whole_resource |= access.whole_resource;
                }
                else
                {
                    accesses[count++] = access;
                }
            }
            accesses.resize(count);
        }
    } // namespace

    void selectQueues(
        const RenderGraph& graph,
        std::array<const QueueProperties*, 3> queues,
        RecordingProgram& recording
    ) noexcept
    {
        for (auto& pass : recording.passes)
        {
            const auto& source = graph.passes()[pass.source_pass];
            const auto* compute = source.work.getIf<ComputeWork>();
            if (!compute || !compute->allow_async_compute || queues[0] == queues[1])
            {
                continue;
            }
            // Imported textures do not yet declare a sharing contract. Keep them on Graphics
            // rather than inventing ownership transfers for externally created EXCLUSIVE images.
            const bool imported_image = std::ranges::any_of(
                source.uses,
                [&](const auto& use)
                {
                    const auto imported = [&](GraphResourceId id)
                    {
                        const auto& resource = graph.resources()[id.index];
                        return resource.origin == EGraphOrigin::IMPORTED &&
                               std::holds_alternative<GraphImageDesc>(resource.description);
                    };
                    return imported(use.version.resource) ||
                           (use.access == EGraphAccess::READ && use.fallback && imported(use.fallback->resource));
                }
            );
            if (!imported_image)
            {
                pass.queue = EQueueRole::COMPUTE;
            }
        }
        // An initial conditional region is a single-queue interval, including its false transitions.
        for (const auto& region : recording.conditions)
        {
            auto passes = std::span(recording.passes).subspan(region.first_pass, region.pass_count);
            if (std::ranges::any_of(passes, [](const auto& pass) { return pass.queue == EQueueRole::GRAPHICS; }))
            {
                for (auto& pass : passes)
                {
                    pass.queue = EQueueRole::GRAPHICS;
                }
            }
        }
    }

    void planSubmissions(
        const RenderGraph& graph,
        const RenderGraphAnalysis& analysis,
        std::span<const CompiledResource> resources,
        RecordingProgram& recording,
        const std::vector<std::vector<std::pair<std::uint32_t, nri::StageBits>>>& use_stages,
        std::array<const QueueProperties*, 3> queues,
        std::vector<SubmissionBatch>& batches,
        std::vector<ImportedAccess>& imports,
        std::uint32_t output
    ) noexcept
    {
        std::vector<std::uint32_t> pass_batches(graph.passes().size(), kGraphInvalidIndex);
        // History initialization and shared preparation copies have a real Graphics submission.
        // Normally it also contains the first recording interval, so the existing one-submit path stays one.
        batches.push_back({});
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            const auto& pass = recording.passes[p];
            if (batches.back().queue != pass.queue)
            {
                batches.push_back({pass.queue, p});
            }
            const auto batch = static_cast<std::uint32_t>(batches.size() - 1);
            recording.passes[p].submission = batch;
            ++batches.back().pass_count;
            pass_batches[pass.source_pass] = batch;
            for (const auto& use : graph.passes()[pass.source_pass].uses)
            {
                const auto add = [&](GraphResourceId resource, bool write)
                {
                    if (resources[resource.index].history != kGraphInvalidIndex)
                    {
                        batches.back().histories.push_back(resource);
                    }
                    // The final Target is also borrowed physical memory, despite its logical
                    // graph resource being transient. A different instance may sample that image.
                    if ((resources[resource.index].origin == EGraphOrigin::IMPORTED || resource.index == output) &&
                        !resources[resource.index].shared_table)
                    {
                        imports.push_back({resource, use.range, write, batch, batch, use.whole_resource});
                    }
                };
                add(use.version.resource, use.access != EGraphAccess::READ);
                if (use.access == EGraphAccess::READ && use.fallback)
                {
                    add(use.fallback->resource, false);
                }
            }
        }
        // Retain the batch which actually touches the range. Folding the entire instance into one
        // write would prevent unrelated prefixes and branches in another instance from progressing.
        compactImportedAccesses(imports);

        // Canonical imported/history/export states are restored on Graphics, outside rendering scopes.
        if (batches.back().queue != EQueueRole::GRAPHICS)
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
            const auto resource = std::ranges::lower_bound(
                uses, hazard.resource.index, {}, [](const auto& use) { return use.first; }
            );
            const bool known = resource != uses.end() && resource->first == hazard.resource.index;
            const auto stages = known ? resource->second : nri::StageBits::ALL;
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
            const auto consumer = pass_batches[pass.source_pass];
            const auto transitionDependency = [&](const auto& barrier)
            {
                if (barrier.previous.index != kGraphInvalidIndex)
                {
                    depend(consumer, pass_batches[barrier.previous.index], barrier.native.after.stages);
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
        // The final boundary must observe every accepted producer, including independent branches.
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
        for (std::uint32_t b = 1; b < batches.size(); ++b)
        {
            depend(b, 0, nri::StageBits::ALL);
            depend(last, b, nri::StageBits::ALL);
        }
        // Object reuse and memory alias proofs include physical-Queue FIFO. Make it an explicit
        // edge before runtime is allowed to select ready batches out of recording-vector order.
        std::array<std::uint32_t, 3> previous_queue_batch{kGraphInvalidIndex, kGraphInvalidIndex, kGraphInvalidIndex};
        for (std::uint32_t b = 0; b < batches.size(); ++b)
        {
            auto q = static_cast<std::size_t>(batches[b].queue);
            for (std::size_t previous = 0; previous < q; ++previous)
            {
                if (queues[previous] == queues[q])
                {
                    q = previous;
                    break;
                }
            }
            if (previous_queue_batch[q] != kGraphInvalidIndex)
            {
                depend(b, previous_queue_batch[q], nri::StageBits::ALL);
            }
            previous_queue_batch[q] = b;
        }

        for (auto& access : imports)
        {
            const auto& resource = resources[access.resource.index];
            if (restored[access.resource.index])
            {
                access.release_batch = last;
            }
            if (std::holds_alternative<GraphImageDesc>(resource.description))
            {
                const auto changesLayout = [&](const ImageTransition& transition)
                {
                    const auto& barrier = transition.native;
                    const auto aspects = barrier.planes == nri::PlaneBits::COLOR   ? 1u
                                         : barrier.planes == nri::PlaneBits::DEPTH ? 2u
                                                                                   : 4u;
                    const GraphImageRange
                        range{aspects, barrier.mipOffset, barrier.mipNum, barrier.layerOffset, barrier.layerNum};
                    return transition.resource == access.resource.index &&
                           barrier.before.layout != barrier.after.layout && overlaps(access.range, range);
                };
                const auto& batch = batches[access.batch];
                for (auto p = batch.first_pass; p < batch.first_pass + batch.pass_count; ++p)
                {
                    access.write |= std::ranges::any_of(recording.passes[p].before.images, changesLayout);
                }
                // A layout transition is exclusive even when the shader only reads the image.
                access.write |= std::ranges::any_of(recording.after.images, changesLayout);
            }
        }
        for (auto& batch : batches)
        {
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
                auto q = static_cast<std::size_t>(batches[dependency.producer].queue);
                for (std::size_t previous = 0; previous < q; ++previous)
                {
                    if (queues[previous] == queues[q])
                    {
                        q = previous;
                        break;
                    }
                }
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
