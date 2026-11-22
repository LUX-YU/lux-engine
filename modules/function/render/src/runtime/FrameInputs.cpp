#include <bit>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    namespace
    {
        constexpr auto no_job = FrameInputs::no_job;

        template <class Index, class Key> std::uint32_t denseIndex(const Index& index, Key key) noexcept
        {
            const auto* id = index.tryGet(key);
            return id ? *id : no_job;
        }

        auto storageOrder(ResourceInstanceId id) noexcept
        {
            return std::pair{id.index, id.gen};
        }
    } // namespace

    void RenderServer::Impl::rebuildFrameInputs(detail::DeviceEntry& device) noexcept
    {
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto started = std::chrono::steady_clock::now();
#endif
        auto& round = device.round;
        auto& inputs = round.inputs;
        inputs.revision = job_structure_revision_;
        round.clearReadSelections();
        inputs.jobs.clear();
        inputs.sources.clear();
        inputs.read_slots.clear();
        inputs.relations.clear();
        inputs.storages.clear();
        inputs.jobs.reserve(running_jobs_.size());
        cxx::SlotKeySparseSet<RenderJobId, std::uint32_t> running;
        cxx::SlotKeySparseSet<TargetId, std::uint32_t> writers, source_slots;
        std::vector<std::vector<ResourceInstanceId>> storage_ids;
        storage_ids.reserve(running_jobs_.size());
        for (auto id : running_jobs_)
        {
            const auto index = static_cast<std::uint32_t>(inputs.jobs.size());
            const auto& job = *runtime_->jobs_.find(id);
            auto& execution = *job.execution;
            if (execution.device != device.id) continue;
            auto& ids = storage_ids.emplace_back();
            auto& entry = inputs.jobs.emplace_back(
                id, job.control.executionGeneration(), execution.target,
                FrameInputs::ReadRange{inputs.read_slots.size(), 0}
            );
            const auto retained = execution.retainedResources();
            entry.retained.assign(retained.begin(), retained.end());
            entry.imports.reserve(execution.target_images.size());
            for (const auto& image : execution.target_images)
            {
                auto index = denseIndex(source_slots, image.target);
                if (index == no_job)
                {
                    index = static_cast<std::uint32_t>(inputs.sources.size());
                    inputs.sources.push_back({image.target});
                    source_slots.insert(image.target, index);
                }
                const FrameInputs::TargetReadSlot source{index};
                const auto reads = inputs.readsFor(entry);
                if (std::ranges::find(reads, source) == reads.end())
                {
                    inputs.read_slots.push_back(source);
                    ++entry.reads.count;
                }
                entry.imports.push_back({image.resource, source});
            }
            running.insert(id, index);
            writers.insert(execution.target, index);
            for (const auto storage : execution.gpu_resources)
            {
                if (storage.isNull()) continue;
                const auto* factory = runtime_->resources_.factory(runtime_->resources_.declarationOf(storage));
                const auto* ops = factory->storage()->bindings();
                if (!ops || !ops->prepareFrame) continue;
                ids.push_back(storage);
                inputs.storages.push_back(storage);
            }
        }
        std::ranges::sort(inputs.storages, {}, storageOrder);
        const auto duplicates = std::ranges::unique(inputs.storages);
        inputs.storages.erase(duplicates.begin(), duplicates.end());
        for (auto& source : inputs.sources) source.writer = denseIndex(writers, source.target);
        const auto count = inputs.jobs.size();
        for (std::uint32_t consumer = 0; consumer < count; ++consumer)
        {
            auto& entry = inputs.jobs[consumer];
            const auto& job = *runtime_->jobs_.find(entry.id);
            for (auto predecessor : job.relations.predecessors)
            {
                const auto producer = denseIndex(running, predecessor);
                if (producer != no_job) inputs.jobs[producer].successors.push_back({consumer, {}});
            }
            for (const auto slot : inputs.readsFor(entry))
            {
                const auto& source = inputs.sources[slot.index];
                if (source.writer != no_job) inputs.jobs[source.writer].successors.push_back({consumer, source.target});
            }
            for (const auto& link : job.execution->storage_bindings)
            {
                if (!link.publication) continue;
                const auto producer = denseIndex(running, link.publication->writer);
                if (producer == no_job) continue;
                auto& edges = inputs.jobs[producer].successors;
                if (std::ranges::find(edges, consumer, &FrameInputs::Edge::consumer) == edges.end())
                    edges.push_back({consumer, {}, true});
            }
            for (auto storage : storage_ids[consumer])
            {
                const auto found = std::ranges::lower_bound(inputs.storages, storageOrder(storage), {}, storageOrder);
                entry.storages.push_back(static_cast<std::uint32_t>(found - inputs.storages.begin()));
            }
        }

        // The full logical order keeps paths through stopped and unselected Jobs available to the projection.
        std::size_t order_capacity = 0;
        const bool has_relations = count != 0 && std::ranges::any_of(runtime_->jobs_, [](const auto& job) noexcept
        {
            return !job.relations.predecessors.empty();
        });
        if (has_relations)
        {
            std::vector<RenderJobId> logical;
            logical.reserve(runtime_->jobs_.size());
            for (const auto& job : runtime_->jobs_) logical.push_back(job.control.id());
            const auto ordered = detail::orderRunningJobs(jobRelations(), logical);
            if (!ordered) std::terminate();
            cxx::SlotKeySparseSet<RenderJobId, std::uint32_t> indices;
            for (auto id : *ordered)
            {
                auto& relation = inputs.relations.emplace_back(denseIndex(running, id));
                for (auto predecessor : runtime_->jobs_.find(id)->relations.predecessors)
                {
                    const auto parent = denseIndex(indices, predecessor);
                    if (parent == no_job) std::terminate();
                    relation.predecessors.push_back(parent);
                }
                indices.insert(id, static_cast<std::uint32_t>(inputs.relations.size() - 1));
            }
            const auto words = (count + 63) / 64;
            inputs.frontiers.resize(words * inputs.relations.size());
            std::ranges::fill(inputs.frontiers, 0);
            // Reserve the transitive upper bound once. The per-round projection may stop at any selected ancestor.
            for (std::size_t i = 0; i < inputs.relations.size(); ++i)
            {
                const auto& relation = inputs.relations[i];
                auto mask = std::span(inputs.frontiers).subspan(i * words, words);
                for (auto predecessor : relation.predecessors)
                {
                    const auto earlier = std::span(inputs.frontiers).subspan(predecessor * words, words);
                    for (std::size_t word = 0; word < words; ++word) mask[word] |= earlier[word];
                    const auto parent = inputs.relations[predecessor].running;
                    if (parent != no_job) mask[parent / 64] |= std::uint64_t{1} << (parent % 64);
                }
                if (relation.running != no_job)
                    for (auto word : mask) order_capacity += std::popcount(word);
            }
        }
        inputs.selection.resize(count);
        inputs.blocked.reserve(2 * count);
        inputs.ordered.reserve(count);
        inputs.used_storages.resize(inputs.storages.size());
        round.reads.resize(inputs.sources.size());
        round.selected_sources.reserve(inputs.sources.size());
        round.outputs.reserve(count);
        round.instances.reserve(count);
        round.storages.reserve(inputs.storages.size());
        round.job_order.reserve(order_capacity);
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto elapsed = std::chrono::steady_clock::now() - started;
        input_rebuilds_.fetch_add(1, std::memory_order_relaxed);
        input_rebuild_ns_.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(), std::memory_order_relaxed
        );
#endif
    }

    RenderError RenderServer::Impl::selectFrameInputs(detail::DeviceEntry& device, std::uint32_t slot) noexcept
    {
        auto& round = device.round;
        auto& inputs = round.inputs;
        using Selection = FrameInputs::Selection;
        using EAvailability = Selection::EAvailability;
        const auto count = inputs.jobs.size();
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto started = std::chrono::steady_clock::now();
        const auto selection_capacity = [&]() noexcept
        {
            return std::array{inputs.selection.capacity(), inputs.blocked.capacity(), inputs.used_storages.capacity(),
                round.outputs.capacity(), round.instances.capacity(), round.storages.capacity(),
                round.selected_sources.capacity()
            };
        };
        const auto topology_capacity = [&]() noexcept
        {
            return std::array{inputs.ordered.capacity(), inputs.frontiers.capacity(), round.job_order.capacity()};
        };
        const auto selection_before = selection_capacity();
        const auto topology_before = topology_capacity();
        cxx::scope_exit record([&]() noexcept
        {
            const auto count_growths = [](const auto& before, const auto& after) noexcept
            {
                std::uint64_t count = 0;
                for (std::size_t i = 0; i < before.size(); ++i) count += before[i] != after[i];
                return count;
            };
            const auto elapsed = std::chrono::steady_clock::now() - started;
            job_selection_ns_.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(), std::memory_order_relaxed
            );
            jobs_considered_.fetch_add(count, std::memory_order_relaxed);
            jobs_selected_.fetch_add(round.outputs.size(), std::memory_order_relaxed);
            dependency_edges_.fetch_add(round.job_order.size(), std::memory_order_relaxed);
            job_selection_growths_.fetch_add(
                count_growths(selection_before, selection_capacity()), std::memory_order_relaxed
            );
            job_topology_growths_.fetch_add(
                count_growths(topology_before, topology_capacity()), std::memory_order_relaxed
            );
            job_selection_calls_.fetch_add(1, std::memory_order_release);
        });
#endif
        round.clearReadSelections();
        round.outputs.clear();
        round.instances.clear();
        round.job_order.clear();
        round.storages.clear();
        inputs.blocked.clear();
        inputs.ordered.clear();
        std::ranges::fill(inputs.selection, Selection{});
        const auto block = [&](std::uint32_t index, RenderError error = {}) noexcept
        {
            auto& state = inputs.selection[index];
            if (!state.active()) return;
            const bool has_new_error = error.type && !state.error.type;
            if (state.ready() || has_new_error) inputs.blocked.push_back(index);
            state.availability = EAvailability::BLOCKED;
            if (has_new_error) state.error = error;
        };
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& entry = inputs.jobs[i];
            auto* live = runtime_->jobs_.find(entry.id);
            if (!live || !live->execution || live->control.executionGeneration() != entry.execution_generation)
            {
                inputs.revision = 0;
                continue;
            }
            auto& job = *live;
            if (!job.readyToRun() || !jobTargetReady(job)) continue;
            auto& state = inputs.selection[i];
            state.availability = EAvailability::READY;
            if (device.preparingProgramsFor(job.execution->binding_identity) ||
                job.execution->programs.pending() || !resourcesAvailable(entry.retained))
            {
                block(i);
                continue;
            }
            const auto& target = *runtime_->targets_.find(entry.target);
            if (const auto* surface = std::get_if<detail::SwapchainTarget>(&target))
            {
                const auto prefetched = surface->prefetched();
                if (!prefetched)
                {
                    job.last_run_error = prefetched.error();
                    job.control.rejectUnadmitted(prefetched.error());
                    state.availability = EAvailability::INACTIVE;
                }
                else if (!*prefetched) block(i);
            }
            else if (!std::get<detail::OffscreenTarget>(target).writable(slot)) block(i);
        }
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (!inputs.selection[i].active()) continue;
            const auto& entry = inputs.jobs[i];
            for (const auto read_slot : inputs.readsFor(entry))
            {
                const auto& read = inputs.sources[read_slot.index];
                const auto source = read.target;
                const auto* target = runtime_->targets_.find(source);
                if (!target)
                {
                    block(i, {kRuntimeIdentity});
                    inputs.selection[i].error = {kRuntimeIdentity};
                }
                else if (targetState(*target).device != device.id) block(i, {kJobState});
                else if (targetState(*target).resizing) block(i);
                else
                {
                    const auto producer = read.writer;
                    if (producer != no_job && inputs.selection[producer].active()) continue;
                    const auto published = publishedOutputSlot(source);
                    if (published == UINT32_MAX)
                    {
                        block(i, {kRuntimeState});
                        inputs.selection[i].error = {kRuntimeState};
                    }
                    else if (!publishedOutputReadable(source, published)) block(i);
                }
            }
        }
        const auto edge_available = [&](const FrameInputs::Edge& edge) noexcept
        {
            if (edge.sampled_target.isNull()) return true;
            const auto* target = runtime_->targets_.find(edge.sampled_target);
            return target && !targetState(*target).resizing;
        };
        const auto block_successor = [&](std::uint32_t producer, const FrameInputs::Edge& edge) noexcept
        {
            if (edge.prior_publication) return;
            if (!edge.sampled_target.isNull())
            {
                const auto history = publishedOutputSlot(edge.sampled_target);
                if (history != UINT32_MAX && publishedOutputReadable(edge.sampled_target, history)) return;
            }
            block(edge.consumer, inputs.selection[producer].error);
        };
        const auto order_jobs = [&](bool include_blocked) noexcept
        {
            const auto participates = [&](const Selection& state) noexcept
            {
                return include_blocked ? state.active() : state.ready();
            };
            inputs.ordered.clear();
            for (auto& state : inputs.selection) state.predecessors = 0;
            std::size_t participants = 0;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (!participates(inputs.selection[i])) continue;
                ++participants;
                for (const auto& edge : inputs.jobs[i].successors)
                    if (participates(inputs.selection[edge.consumer]) && edge_available(edge))
                        ++inputs.selection[edge.consumer].predecessors;
            }
            for (std::uint32_t i = 0; i < count; ++i)
                if (participates(inputs.selection[i]) && inputs.selection[i].predecessors == 0)
                    inputs.ordered.push_back(i);
            for (std::size_t cursor = 0; cursor < inputs.ordered.size(); ++cursor)
            {
                const auto index = inputs.ordered[cursor];
                for (const auto& edge : inputs.jobs[index].successors)
                {
                    auto& consumer = inputs.selection[edge.consumer];
                    if (!participates(consumer) || !edge_available(edge)) continue;
                    if (!inputs.selection[index].ready()) block_successor(index, edge);
                    if (--consumer.predecessors == 0) inputs.ordered.push_back(edge.consumer);
                }
            }
            return inputs.ordered.size() == participants;
        };

        // Retain the running order's tie-breaks through an ordinary blocked predecessor.
        // Across a cycle, propagate availability first and only test the candidates that can actually run.
        bool acyclic = order_jobs(true);
        if (!acyclic)
        {
            for (std::size_t i = 0; i < inputs.blocked.size(); ++i)
            {
                const auto index = inputs.blocked[i];
                for (const auto& edge : inputs.jobs[index].successors)
                    if (edge_available(edge)) block_successor(index, edge);
            }
            acyclic = order_jobs(false);
        }
        if (acyclic)
        {
            for (auto index : inputs.ordered)
            {
                if (inputs.selection[index].ready())
                {
                    const auto& execution = *runtime_->jobs_.find(inputs.jobs[index].id)->execution;
                    auto pinned = runtime_->resources_.pinBufferUses(execution.productUses(), slot);
                    if (!pinned || !*pinned) block(index, pinned ? RenderError{} : pinned.error());
                }
                if (!inputs.selection[index].ready())
                {
                    for (const auto& edge : inputs.jobs[index].successors)
                        if (edge_available(edge)) block_successor(index, edge);
                }
            }
        }
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& state = inputs.selection[i];
            if (state.error.type) runtime_->jobs_.find(inputs.jobs[i].id)->control.rejectUnadmitted(state.error);
        }
        if (!acyclic) return {kGraphDependencyCycle};
        const auto blocked = std::ranges::remove_if(inputs.ordered, [&](auto index) noexcept
        {
            return !inputs.selection[index].ready();
        });
        inputs.ordered.erase(blocked.begin(), blocked.end());
        if (inputs.ordered.empty()) return {};
        for (std::uint32_t i = 0; i < inputs.ordered.size(); ++i) inputs.selection[inputs.ordered[i]].selected = i;

        std::ranges::fill(inputs.used_storages, std::uint8_t{});
        for (auto index : inputs.ordered)
        {
            const auto& job = inputs.jobs[index];
            round.outputs.push_back({job.id, job.target, job.execution_generation, index});
            for (const auto read_slot : inputs.readsFor(job))
            {
                auto& selected = round.reads[read_slot.index];
                if (!selected.publication.target.isNull()) continue;
                const auto& source = inputs.sources[read_slot.index];
                const auto producer = source.writer == no_job ? no_job : inputs.selection[source.writer].selected;
                selected = {{source.target, kGraphInvalidIndex, 0, 0}, producer};
                round.selected_sources.push_back(read_slot);
            }
            for (auto storage : job.storages) inputs.used_storages[storage] = 1;
        }
        for (std::size_t i = 0; i < inputs.storages.size(); ++i)
            if (inputs.used_storages[i]) round.storages.push_back(inputs.storages[i]);

        // Project the nearest selected ancestors in one logical topological pass.
        // A dormant Job folds its own frontier into its successors.
        const auto words = (count + 63) / 64;
        for (std::size_t i = 0; i < inputs.relations.size(); ++i)
        {
            const auto& relation = inputs.relations[i];
            auto mask = std::span(inputs.frontiers).subspan(i * words, words);
            std::ranges::fill(mask, 0);
            for (auto predecessor : relation.predecessors)
            {
                const auto parent = inputs.relations[predecessor].running;
                if (parent != no_job && inputs.selection[parent].selected != no_job)
                {
                    mask[parent / 64] |= std::uint64_t{1} << (parent % 64);
                }
                else
                {
                    const auto earlier = std::span(inputs.frontiers).subspan(predecessor * words, words);
                    for (std::size_t word = 0; word < words; ++word) mask[word] |= earlier[word];
                }
            }
            if (relation.running == no_job) continue;
            const auto consumer = inputs.selection[relation.running].selected;
            if (consumer == no_job) continue;
            for (std::size_t word = 0; word < words; ++word)
            {
                for (auto bits = mask[word]; bits; bits &= bits - 1)
                {
                    const auto producer = word * 64 + std::countr_zero(bits);
                    round.job_order.push_back({inputs.selection[producer].selected, consumer});
                }
            }
        }
        std::ranges::sort(round.job_order, [](const auto& left, const auto& right) noexcept
        {
            return std::tie(left.consumer, left.producer) < std::tie(right.consumer, right.producer);
        });
        return {};
    }
} // namespace lux::render
