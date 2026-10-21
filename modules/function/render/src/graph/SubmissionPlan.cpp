#include <algorithm>
#include <lux/engine/render/graph/SubmissionPlan.hpp>
#include <tuple>

namespace lux::render::detail
{
    namespace
    {
        void compactImportedAccesses(std::vector<ImportedAccess>& accesses) noexcept
        {
            std::ranges::sort(
                accesses,
                [](const auto& a, const auto& b)
                {
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
                const bool is_duplicate = count != 0 && accesses[count - 1].resource == access.resource &&
                                          accesses[count - 1].range == access.range;
                if (is_duplicate)
                {
                    accesses[count - 1].write |= access.write;
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
            const auto* compute = std::get_if<ComputeWork>(&source.work);
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
        const RecordingProgram& recording,
        std::vector<SubmissionBatch>& batches,
        std::vector<ImportedAccess>& imports
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
                    if (resources[resource.index].origin == EGraphOrigin::IMPORTED &&
                        !resources[resource.index].shared_table)
                    {
                        imports.push_back({resource, use.range, write});
                    }
                };
                add(use.version.resource, use.access != EGraphAccess::READ);
                if (use.access == EGraphAccess::READ && use.fallback)
                {
                    add(use.fallback->resource, false);
                }
            }
        }
        // Cross-instance ordering needs each exact range once, with all pass writes folded in.
        // Distinct ranges and logical resources remain separate until their physical bindings are known.
        compactImportedAccesses(imports);

        // Canonical imported/history/export states are restored on Graphics, outside rendering scopes.
        if (batches.back().queue != EQueueRole::GRAPHICS)
        {
            batches.push_back({EQueueRole::GRAPHICS, static_cast<std::uint32_t>(recording.passes.size())});
        }
        const auto depend = [&](std::uint32_t consumer, std::uint32_t producer)
        {
            if (consumer != producer)
            {
                batches[consumer].dependencies.push_back({producer});
            }
        };
        for (const auto& edge : analysis.dependencies)
        {
            const auto before = pass_batches[edge.before.index], after = pass_batches[edge.after.index];
            if (before != kGraphInvalidIndex && after != kGraphInvalidIndex)
            {
                depend(after, before);
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
                    depend(consumer, pass_batches[barrier.previous.index]);
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
        const auto finalHistory = [&](const auto& transition)
        {
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
            depend(b, 0);
            depend(last, b);
        }
        for (auto& batch : batches)
        {
            std::ranges::sort(batch.dependencies, {}, &SubmissionDependency::producer);
            batch.dependencies.erase(
                std::unique(
                    batch.dependencies.begin(),
                    batch.dependencies.end(),
                    [](const auto& a, const auto& b) { return a.producer == b.producer; }
                ),
                batch.dependencies.end()
            );
            std::ranges::sort(batch.histories, {}, &GraphResourceId::index);
            batch.histories.erase(std::unique(batch.histories.begin(), batch.histories.end()), batch.histories.end());
        }
    }
} // namespace lux::render::detail
