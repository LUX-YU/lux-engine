#include <ranges>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/graph/StageScope.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    namespace
    {
        constexpr auto no_job = FrameInputs::no_job;
    } // namespace

    bool FrameInputs::Job::uses(ResourceInstanceId id) const noexcept
    {
        const auto key = detail::resourceOrder(id);
        return std::ranges::binary_search(direct_resources, key, {}, detail::resourceOrder) ||
            std::ranges::binary_search(indirect_resources, key, {}, detail::resourceOrder);
    }

    std::uint32_t FrameInputs::storageSlot(ResourceInstanceId id) const noexcept
    {
        const auto order = [](const auto& resource) noexcept { return detail::resourceOrder(resource.id); };
        const auto found = std::ranges::lower_bound(storages, detail::resourceOrder(id), {}, order);
        if (found == storages.end() || found->id != id) return UINT32_MAX;
        return static_cast<std::uint32_t>(found - storages.begin());
    }

    std::optional<FrameInputs::TargetReadSlot>
    RenderServer::Impl::resolveTargetRead(FrameInputs& inputs, RasterTargetImage image) noexcept
    {
        const bool current = image.read == EProductRead::CURRENT_RUN;
        auto& slots = inputs.source_slots[current];
        if (const auto* found = slots.tryGet(image.target)) return FrameInputs::TargetReadSlot{*found};
        auto* entry = runtime_->targets_.find(image.target);
        const auto* target = std::get_if<detail::OffscreenTarget>(entry);
        if (!target || !target->description().sampled) return std::nullopt;
        const auto index = inputs.sources.size();
        const auto* writer = current ? inputs.writers.tryGet(image.target) : nullptr;
        inputs.sources.push_back({image.target, image.read, *entry, writer ? *writer : no_job, {}});
        slots.insert(image.target, index);
        return FrameInputs::TargetReadSlot{index};
    }

    void FrameInputs::releaseTargetReads() noexcept
    {
        for (auto slot : selected_sources) sources[slot.index].selected = {};
        selected_sources.clear();
        while (sources.size() > static_sources)
        {
            const auto& row = sources.back();
            source_slots[row.read == EProductRead::CURRENT_RUN].erase(row.target);
            sources.pop_back();
        }
        for (auto i : jobs_with_target_lists) jobs[i].imports.resize(jobs[i].static_imports);
    }

    void RenderServer::Impl::rebuildFrameInputs(detail::DeviceEntry& device) noexcept
    {
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto started = std::chrono::steady_clock::now();
#endif
        auto& round = device.round;
        auto& inputs = round.inputs;
        inputs.revision = job_structure_revision_;
        inputs.jobs.clear();
        inputs.sources.clear();
        inputs.writers.clear();
        inputs.storages.clear();
        inputs.views.clear();
        inputs.jobs.reserve(running_jobs_.size());
        for (auto& slots : inputs.source_slots) slots.clear();
        inputs.jobs_with_target_lists.clear();
        for (auto id : running_jobs_)
        {
            const auto index = static_cast<std::uint32_t>(inputs.jobs.size());
            const auto& job = *runtime_->jobs_.find(id);
            auto& execution = *job.execution;
            if (execution.device != device.id) continue;
            auto& target = *runtime_->targets_.find(execution.target);
            auto& entry = inputs.jobs.emplace_back(id, job.executionGeneration(), target);
            entry.direct_resources = job.resources;
            entry.indirect_resources = execution.indirect_resources;
            if (std::ranges::any_of(execution.draw_list_sources, [](const auto& source)
                { return !source.stream.target_arguments.empty(); }))
                inputs.jobs_with_target_lists.push_back(index);
            inputs.writers.insert(execution.target, index);
            inputs.storages.insert(inputs.storages.end(), execution.frame_resources.begin(), execution.frame_resources.end());
        }
        const auto resource_order = [](const detail::InstalledFrameResource& resource) noexcept
        {
            return detail::resourceOrder(resource.id);
        };
        std::ranges::sort(inputs.storages, {}, resource_order);
        const auto duplicates = std::ranges::unique(inputs.storages, {}, &detail::InstalledFrameResource::id);
        inputs.storages.erase(duplicates.begin(), duplicates.end());
        const auto count = inputs.jobs.size();
        inputs.window_jobs.reserve(count);
        std::vector<std::uint32_t> view_slots;
        for (auto& entry : inputs.jobs)
        {
            const auto& execution = *runtime_->jobs_.find(entry.id)->execution;
            // All writers are registered before a CURRENT_RUN operand resolves its producer.
            entry.imports.reserve(execution.target_images.size());
            for (const auto& image : execution.target_images)
            {
                const auto source = resolveTargetRead(inputs, {image.target, image.read});
                if (!source) std::terminate(); // A live installed input retains its sampled Target.
                entry.imports.push_back({&image.operand, *source});
            }
            entry.static_imports = entry.imports.size();
            entry.storages.reserve(execution.frame_resources.size());
            for (const auto& source : execution.frame_resources) entry.storages.push_back(inputs.storageSlot(source.id));
            entry.view_slots.reserve(execution.view_sources.size());
            if (execution.view_sources.empty()) continue;
            if (view_slots.empty()) view_slots.resize(inputs.storages.size(), kGraphInvalidIndex);
            for (const auto local : execution.view_sources)
            {
                const auto storage = entry.storages[local];
                auto& slot = view_slots[storage];
                if (slot == kGraphInvalidIndex)
                {
                    const auto& owner = inputs.storages[storage];
                    slot = static_cast<std::uint32_t>(inputs.views.size());
                    inputs.views.push_back({owner.instance, owner.ops->cpu_view->select});
                }
                entry.view_slots.push_back(slot);
            }
        }

        inputs.static_sources = inputs.sources.size();
        inputs.selection.resize(count);
        inputs.used_storages.resize(inputs.storages.size());
        inputs.selected_sources.reserve(inputs.sources.size());
        round.outputs.reserve(count);
        round.executions.reserve(count);
        round.resource_uploads.reserve(inputs.storages.size());
        std::size_t order_capacity = 0;
        for (const auto& job : inputs.jobs) order_capacity += job.imports.size();
        round.job_order.reserve(order_capacity);
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto elapsed = std::chrono::steady_clock::now() - started;
        input_rebuilds_.fetch_add(1, std::memory_order_relaxed);
        input_rebuild_ns_.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(), std::memory_order_relaxed
        );
#endif
    }

    RenderResult<std::uint32_t>
    FrameInputs::linkDrawRead(Job& job, detail::FrameResources& frame, ImageImport input) noexcept
    {
        const auto source = input.source.index;
        if (source >= imports_by_source.size()) imports_by_source.resize(source + 1);
        auto& linked = imports_by_source[source];
        if (linked.import_index == kGraphInvalidIndex)
        {
            if (job.imports.size() >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kDeviceCapacity});
            linked.import_index = static_cast<std::uint32_t>(job.imports.size());
            job.imports.push_back(input);
        }
        else
        {
            auto& first = job.imports[linked.import_index];
            first.wait_stages = detail::combineStages(first.wait_stages, input.wait_stages);
        }
        // Raster sources arrive in graphics batch order. Every touching batch keeps an
        // acceptance record, while all its draws refer to the same selected publication.
        auto& reads = frame.target_reads;
        if (linked.last_read == kGraphInvalidIndex || reads[linked.last_read].batch != input.batch)
        {
            if (reads.size() >= kGraphInvalidIndex) return cxx::unexpected(RenderError{kDeviceCapacity});
            linked.last_read = static_cast<std::uint32_t>(reads.size());
            reads.push_back({input.batch, linked.import_index});
        }
        return linked.last_read;
    }

    RenderResult<bool>
    RenderServer::Impl::appendDrawReads(FrameInputs& inputs, std::uint32_t reader, std::uint32_t slot) noexcept
    {
        auto& job = inputs.jobs[reader];
        auto& execution = *runtime_->jobs_.find(job.id)->execution;
        auto& frame = execution.frame(slot);
        frame.target_reads.clear();
        auto& operands = frame.target_bindings;
        operands.clear();
        inputs.imports_by_source.clear();
        bool ordered_lists = true;
        for (const auto& source : execution.draw_list_sources)
        {
            if (source.stream.target_arguments.empty() || !execution.passEnabledThisRun(source.stream.pass)) continue;
            const auto data = source.read(source.owner, std::nullopt);
            if (!data.fitsSlotRanges()) return cxx::unexpected(RenderError{kDeviceCapacity});
            // Draws may repeat or overlap large binding slices. Sweep the union in ordinal order
            // so each referenced binding is resolved once, including when draws arrive out of order.
            auto& ends = inputs.binding_ends;
            ends.clear();
            for (const auto& draw : data.draws)
            {
                const auto range = draw.bindings;
                if (!range.fits(data.bindings.size())) return cxx::unexpected(RenderError{kFrameContract});
                if (!range.count) continue;
                if (range.first >= ends.size()) ends.resize(std::size_t{range.first} + 1);
                ends[range.first] = std::max(ends[range.first], range.first + range.count);
            }
            const auto list_begin = operands.size();
            std::uint32_t visited = 0;
            for (std::uint32_t first = 0; first < ends.size(); ++first)
            {
                const auto end = ends[first];
                for (auto n = std::max(first, visited); n < end; ++n)
                {
                    const auto& binding = data.bindings[n];
                    const auto* image = std::get_if<RasterTargetImage>(&binding.source);
                    if (!image) continue;
                    const bool valid_mode = image->read == EProductRead::CURRENT_RUN ||
                        image->read == EProductRead::LATEST_PUBLISHED;
                    if (!valid_mode || binding.argument >= source.stream.target_arguments.size())
                        return cxx::unexpected(RenderError{kFrameContract, {source.stream.pass, n}});
                    const auto product = resolveTargetRead(inputs, *image);
                    if (!product) return cxx::unexpected(RenderError{kRuntimeIdentity});
                    const auto& row = inputs.sources[product->index];
                    const auto& description = row.owner().description();
                    const auto& argument = source.stream.target_arguments[binding.argument];
                    if (argument.format != description.format || description.samples != 1)
                        return cxx::unexpected(RenderError{kFrameContract, {source.stream.pass, n}});
                    const FrameInputs::ImageImport input{nullptr, *product, source.stream.batch, argument.stages};
                    const auto read = inputs.linkDrawRead(job, frame, input);
                    if (!read) return cxx::unexpected(read.error());
                    operands.push_back({source.stream.list, n, binding.argument, *read});
                }
                visited = std::max(visited, end);
            }
            if (list_begin && list_begin < operands.size())
                ordered_lists &= operands[list_begin - 1].list < source.stream.list;
        }
        if (!ordered_lists) std::ranges::sort(operands, {}, &detail::FrameResources::TargetBinding::key);
        const auto dynamic = std::span{job.imports}.subspan(job.static_imports);
        return std::ranges::any_of(dynamic, [&](const auto& import) noexcept
        {
            const auto writer = inputs.sources[import.source.index].writer;
            return writer != no_job && writer >= reader && inputs.selection[writer].ready();
        });
    }

    void FrameInputs::orderConsumers() noexcept
    {
        product_order.begin(jobs.size());
        for (std::uint32_t i = 0; i < jobs.size(); ++i)
        {
            if (!selection[i].ready()) continue;
            for (const auto& import : jobs[i].imports)
            {
                const auto writer = sources[import.source.index].writer;
                if (writer != no_job && selection[writer].ready()) product_order.addRead(writer, i);
            }
        }
        if (product_order.finish()) return;
        for (std::uint32_t i = 0; i < jobs.size(); ++i)
            if (product_order.blocked(i)) selection[i].block({kJobCycle});
    }

    void RenderServer::Impl::qualifyTargetReads(
        detail::DeviceEntry& device, std::uint32_t index, bool collect_producers
    ) noexcept
    {
        auto& inputs = device.round.inputs;
        auto& state = inputs.selection[index];
        for (const auto& import : inputs.jobs[index].imports)
        {
            const auto& read = inputs.sources[import.source.index];
            const auto& target = read.owner();
            if (target.state.device != device.id) state.block({kJobState});
            else if (target.state.pending_replacement.has_value()) state.block();
            else if (read.read == EProductRead::CURRENT_RUN)
            {
                if (read.writer == no_job || !inputs.selection[read.writer].active()) state.block({kRuntimeState});
                else if (!inputs.selection[read.writer].ready()) state.block(inputs.selection[read.writer].error);
                else if (!runtime_->jobs_.find(inputs.jobs[read.writer].id)->execution->writesTargetThisRun())
                    state.block({kRuntimeState});
                else if (collect_producers && !std::exchange(inputs.selection[read.writer].checking_window, true))
                    inputs.window_jobs.push_back(read.writer);
            }
            else if (target.publishedImage() == UINT32_MAX)
                state.block({kRuntimeState});
        }
    }

    RenderResult<bool> RenderServer::Impl::qualifyWindowTargets(detail::DeviceEntry& device) noexcept
    {
        auto& inputs = device.round.inputs;
        auto& jobs = inputs.window_jobs;
        // The prefix contains the requested Runs. Look through their installed CURRENT_RUN inputs before adoption.
        const auto members = jobs.size();
        for (auto i : jobs) inputs.selection[i].checking_window = true;
        cxx::scope_exit restore([&]() noexcept
        {
            for (auto i : jobs) inputs.selection[i].checking_window = false;
            jobs.resize(members);
        });

        bool ready = true;
        for (std::size_t n = 0; n < jobs.size(); ++n)
        {
            const auto i = jobs[n];
            auto& state = inputs.selection[i];
            if (state.ready()) qualifyTargetReads(device, i, true);
            if (state.error.type) return cxx::unexpected(state.error);
            ready &= state.ready();
            if (n < members || !state.ready()) continue;
            const auto& writer = *runtime_->jobs_.find(inputs.jobs[i].id);
            if (!writer.readyToRun()) return cxx::unexpected(RenderError{kRuntimeState});
            if (writer.waitsForAdoption()) return cxx::unexpected(RenderError{kJobBusy});
        }
        return ready;
    }

    void RenderServer::Impl::adoptReadyWindows(detail::DeviceEntry& device, detail::FrameContext frame) noexcept
    {
        auto& round = device.round;
        auto& inputs = round.inputs;
        const auto slot = frame.slot_index;
        for (auto& window : input_windows_)
        {
            auto* lead = runtime_->jobs_.find(window.lead.job);
            if (!lead || !lead->waitsForAdoption() || !lead->awaits({window.requester, window.lead.reply}) ||
                lead->execution->device != device.id)
                continue;

            auto& members = inputs.window_jobs;
            members.clear();
            window.forEachRun([&](const auto& run) noexcept
            {
                const auto& job = *runtime_->jobs_.find(run.job);
                const auto* index = inputs.writers.tryGet(job.execution->target);
                if (!index) std::terminate(); // Registered Runs retain their installed target.
                members.push_back(*index);
            });

            if (round.active())
            {
                for (const auto i : members) inputs.selection[i].block();
                continue;
            }
            auto& updates = lead->pendingUpdates();
            const auto available = updates.visit([&](const auto& update) noexcept -> RenderResult<void>
            {
                bool busy;
                if constexpr (std::same_as<std::remove_cvref_t<decltype(update)>, FrameData>)
                    busy = awaitingInputRead(update);
                else busy = awaitingInputRead(update.command, update.request);
                if (busy) return cxx::unexpected(RenderError{kDeviceBusy});
                return {};
            });
            const auto targets = qualifyWindowTargets(device);
            bool ready = available && targets && *targets;
            RenderError failure = targets ? RenderError{} : targets.error();
            std::size_t pinned_members = 0;
            if (ready)
            {
                for (const auto i : members)
                {
                    const auto& job = *runtime_->jobs_.find(inputs.jobs[i].id);
                    const auto pinned = runtime_->resources_.pinBufferUses(job.execution->buffer_products, slot);
                    if (!pinned || !*pinned)
                    {
                        if (!pinned) failure = pinned.error();
                        ready = false;
                        break;
                    }
                    ++pinned_members;
                }
            }
            if (!ready)
            {
                for (const auto i : std::span{members}.first(pinned_members))
                    ResourceRegistry::unpinBufferUses(runtime_->jobs_.find(inputs.jobs[i].id)->execution->buffer_products);
                for (const auto i : members)
                {
                    inputs.selection[i].block(failure);
                    if (failure.type) runtime_->jobs_.find(inputs.jobs[i].id)->rejectUnadmitted(failure);
                }
                continue;
            }

            round.begin(frame, device.uploader());
            for (const auto i : members)
            {
                runtime_->jobs_.find(inputs.jobs[i].id)->adoptRequest();
                inputs.selection[i].window_pinned = true;
                for (const auto resource : inputs.jobs[i].storages)
                    inputs.used_storages[resource] = FrameInputs::EResourceUse::WINDOW_MEMBER;
            }
            if (auto adopted = applyWindowInputs(updates, inputs); !adopted)
                for (const auto i : members) inputs.selection[i].block(adopted.error());
        }
    }

    void RenderServer::Impl::selectFrameInputs(detail::DeviceEntry& device, detail::FrameContext frame) noexcept
    {
        auto& round = device.round;
        auto& inputs = round.inputs;
        const auto slot = frame.slot_index;
        using Selection = FrameInputs::Selection;
        using EAvailability = Selection::EAvailability;
        const auto count = inputs.jobs.size();
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto started = std::chrono::steady_clock::now();
        const auto selection_capacity = [&]() noexcept
        {
            return std::array{inputs.selection.capacity(), inputs.used_storages.capacity(),
                round.outputs.capacity(), round.executions.capacity(), round.resource_uploads.capacity(),
                inputs.selected_sources.capacity()
            };
        };
        const auto selection_before = selection_capacity();
        const auto topology_before = round.job_order.capacity();
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
                round.job_order.capacity() != topology_before, std::memory_order_relaxed
            );
            job_selection_calls_.fetch_add(1, std::memory_order_release);
        });
#endif
        round.job_order.clear();
        round.resource_uploads.clear();
        std::ranges::fill(inputs.selection, Selection{});
        std::ranges::fill(inputs.used_storages, FrameInputs::EResourceUse::UNUSED);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& entry = inputs.jobs[i];
            auto* live = runtime_->jobs_.find(entry.id);
            if (!live || !live->execution || live->executionGeneration() != entry.execution_generation)
            {
                inputs.revision = 0;
                continue;
            }
            auto& job = *live;
            if (!job.readyToRun() || !targetReadyFor(entry.target, job.id())) continue;
            auto& state = inputs.selection[i];
            state.availability = EAvailability::READY;
            const bool targets_resizing = targetState(entry.target).pending_replacement.has_value() ||
                std::ranges::any_of(entry.imports, [&](const auto& import) noexcept
                {
                    return inputs.sources[import.source.index].owner().state.pending_replacement.has_value();
                });
            if (targets_resizing || !resourcesAvailable(entry))
            {
                inputs.selection[i].block();
                continue;
            }
            const auto& target = entry.target;
            if (const auto* surface = std::get_if<detail::SwapchainTarget>(&target))
            {
                const auto prefetched = surface->prefetched();
                if (!prefetched)
                {
                    job.last_run_error = prefetched.error();
                    rejectPendingRun(job, prefetched.error());
                    state.availability = EAvailability::INACTIVE;
                }
                else if (!*prefetched) inputs.selection[i].block();
            }
            else
            {
                state.output_image = std::get<detail::OffscreenTarget>(target).writableImage(slot);
                if (state.output_image == UINT32_MAX) inputs.selection[i].block();
            }
        }
        adoptReadyWindows(device, frame);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (!inputs.selection[i].ready()) continue;
            const auto& entry = inputs.jobs[i];
            if (!runtime_->jobs_.find(entry.id)->readyToRun())
            {
                inputs.selection[i].block();
                continue;
            }
            const bool missing_input = std::ranges::any_of(entry.storages, [&](auto index) noexcept
            {
                const auto& resource = inputs.storages[index];
                return resource.ops->hasRequiredInput && !resource.ops->hasRequiredInput(resource.instance);
            });
            if (missing_input)
            {
                inputs.selection[i].block({kFrameContract});
            }
        }
        bool needs_sort = false;
        for (auto i : inputs.jobs_with_target_lists)
        {
            if (!inputs.selection[i].ready()) continue;
            auto reads = appendDrawReads(inputs, i, slot);
            if (!reads) inputs.selection[i].block(reads.error());
            else needs_sort |= *reads;
        }
        // Static imports and forward dynamic reads already follow the installed order.
        if (needs_sort) inputs.orderConsumers();
        const auto reordered = inputs.product_order.ordered();
        const auto ordered_count = static_cast<std::uint32_t>(needs_sort ? reordered.size() : count);
        const auto ordered_jobs = std::views::iota(0u, ordered_count) | std::views::transform([&](std::uint32_t i)
        {
            return needs_sort ? reordered[i] : i;
        });
        for (const auto i : ordered_jobs)
        {
            if (!inputs.selection[i].ready()) continue;
            const auto& entry = inputs.jobs[i];
            qualifyTargetReads(device, i);
            if (!inputs.selection[i].window_pinned && inputs.selection[i].ready())
            {
                auto& execution = *runtime_->jobs_.find(entry.id)->execution;
                auto pinned = runtime_->resources_.pinBufferUses(execution.buffer_products, slot);
                if (!pinned || !*pinned) inputs.selection[i].block(pinned ? RenderError{} : pinned.error());
            }
        }
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto& state = inputs.selection[i];
            if (state.ready()) continue;
            // The packet may already have changed Persistent owners. A post-adoption failure
            // settles this exact Run once, including temporary product contention.
            if (!state.error.type && !state.window_pinned) continue;
            auto& job = *runtime_->jobs_.find(inputs.jobs[i].id);
            const auto failure = state.error.type ? state.error : RenderError{kJobBusy};
            if (state.window_pinned)
            {
                ResourceRegistry::unpinBufferUses(job.execution->buffer_products);
                job.last_run_error = failure;
            }
            job.rejectUnadmitted(failure);
        }
        for (const auto index : ordered_jobs)
        {
            if (!inputs.selection[index].ready()) continue;
            const auto consumer = static_cast<std::uint32_t>(round.outputs.size());
            inputs.selection[index].selected = consumer;
            const auto& job = inputs.jobs[index];
            round.outputs.push_back({job, inputs.selection[index].output_image});
            for (const auto& import : job.imports)
            {
                const auto read_slot = import.source;
                auto& source = inputs.sources[read_slot.index];
                const bool current = source.read == EProductRead::CURRENT_RUN;
                if (!source.selected.input.image)
                {
                    std::optional<std::uint32_t> writer_image;
                    if (current) writer_image = inputs.selection[source.writer].output_image;
                    source.selected = source.owner().selectRead(writer_image);
                    inputs.selected_sources.push_back(read_slot);
                }
                if (!current) continue;
                const auto writer = inputs.selection[source.writer].selected;
                if (!import.operand) round.job_order.push_back({writer, consumer, import.batch, import.wait_stages});
                else
                {
                    for (const auto& wait : import.operand->first_reads)
                        if (wait.first_batch != kGraphInvalidIndex)
                            round.job_order.push_back({writer, consumer, wait.first_batch, wait.stages});
                }
            }
            for (auto storage : job.storages) inputs.used_storages[storage] = FrameInputs::EResourceUse::GRAPH;
        }
    }
} // namespace lux::render
