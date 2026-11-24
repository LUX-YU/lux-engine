#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/runtime/DeviceEntry.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    RenderResult<void> RenderServer::Impl::adoptFrameData(const FrameData& packet, FrameInputs* inputs) noexcept
    {
        if (!packet) return cxx::unexpected(RenderError{kFrameContract});
        auto& registry = runtime_->resources_;

        // Owner-only, synchronous borrow window. Reserve every participating resource before the first write.
        cxx::scope_exit release(
            [&]() noexcept
            {
                for (const auto& update : selected_updates_) runtime_detail::finishResourceCommand(registry, update.id);
                selected_updates_.clear();
            }
        );

        for (const auto& batch : packet.batches())
        {
            if (batch.target.server != instance_id_) return cxx::unexpected(RenderError{kFrameContract});

            detail::FrameDataCall call{};
            const auto selected = registry.selectFrameUpdate(batch, packet, call);
            if (!selected) return cxx::unexpected(selected.error());
            if (!*selected) continue;
            auto& update = selected_updates_.emplace_back(call);
            if (inputs && update.per_run)
            {
                const auto slot = inputs->storageSlot(update.id);
                if (slot == UINT32_MAX || inputs->used_storages[slot] == FrameInputs::EResourceUse::UNUSED)
                    return cxx::unexpected(RenderError{kFrameContract});
                update.frame_resource = slot;
            }
        }
        for (const auto& update : selected_updates_)
        {
            if (auto applied = update.apply(update.owner, packet, update.first_column); !applied) return applied;
            if (update.frame_resource != SIZE_MAX)
                inputs->used_storages[update.frame_resource] = FrameInputs::EResourceUse::INPUT_ONLY;
        }
        return {};
    }

    RenderResult<void> RenderServer::Impl::applyWindowInputs(WindowUpdates& updates, FrameInputs& inputs) noexcept
    {
        return updates.visit([&](auto& update) noexcept -> RenderResult<void>
        {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(update)>, FrameData>)
                return adoptFrameData(update, &inputs);
            else return applyWindowCommand(update, inputs);
        });
    }

    RenderResult<void> RenderServer::Impl::applyWindowCommand(WindowCommand& update, FrameInputs& inputs) noexcept
    {
        const auto* entry = runtime_->command_registry_.findCommand(update.command);
        if (!entry) return cxx::unexpected(RenderError{kCommandContract});
        const auto& desc = *entry->description;
        const bool is_input = entry->effect || entry->storage || desc.name.name() == UpdateView::name;
        const bool valid = is_input && !desc.requires_idle_resource && !desc.failure_safe &&
            desc.response == cxx::make_basic_type_info<AckResponse>() && update.request.typeHash() == desc.request.hash();
        if (!valid) return cxx::unexpected(RenderError{kCommandContract});
        ipc::IpcMessage response;
        CommandDispatchContext context{
            update.request, {}, response_outbox_, command_scope_,
            server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery), runtime_->resources_, {}, nullptr, &response
        };
        std::uint32_t per_run_slot = UINT32_MAX;
        if (entry->storage)
        {
            const auto id = entry->storage(update.request);
            const auto* factory = runtime_->resources_.factoryOf(id);
            if (!factory) return cxx::unexpected(RenderError{kStorageIdentity});
            if (factory->descriptor->lifetime == EResourceContentLifetime::PER_RUN)
            {
                per_run_slot = inputs.storageSlot(id);
                if (per_run_slot == UINT32_MAX || inputs.used_storages[per_run_slot] == FrameInputs::EResourceUse::UNUSED)
                    return cxx::unexpected(RenderError{kFrameContract});
            }
        }
        auto call = resolveCommandCall(update.command, *entry, context);
        if (!call) return cxx::unexpected(call.error());
        if (!call->synchronous)
        {
            if (context.resource) finishResourceCommand(runtime_->resources_, context.resource_id);
            return cxx::unexpected(RenderError{kCommandContract});
        }
        call->invoke(call->object, context);
        const auto error = response.get<AckResponse>().error;
        if (error.type) return cxx::unexpected(error);
        if (per_run_slot != UINT32_MAX) inputs.used_storages[per_run_slot] = FrameInputs::EResourceUse::INPUT_ONLY;
        return {};
    }

    UpdateFrameData::Response RenderServer::Impl::updateFrameData(UpdateFrameData::Request&& input) noexcept
    {
        const auto result = adoptFrameData(input.data);
        return {result ? RenderError{} : result.error(), std::move(input.data)};
    }

    bool RenderServer::Impl::beginFrame(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        const auto& inputs = round.inputs;
        const bool structure_changed = inputs.revision != job_structure_revision_;
        if (structure_changed) rebuildFrameInputs(entry);
        if (inputs.jobs.empty()) return false;
        const auto available = entry.frameProgress().availableFrame(entry.session());
        if (!available)
        {
            for (const auto& candidate : inputs.jobs)
                if (auto* job = runtime_->jobs_.find(candidate.id)) job->rejectUnadmitted(available.error());
            return false;
        }
        if (!*available) return false;

        const auto context = **available;
        selectFrameInputs(entry, context);
        if (!round.active())
        {
            if (round.outputs.empty())
            {
                round.inputs.releaseTargetReads();
                return false;
            }
            round.begin(context, entry.uploader());
        }
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        round.opened_at = std::chrono::steady_clock::now();
        rounds_opened_.fetch_add(1, std::memory_order_relaxed);
#endif
        for (const auto& output : round.outputs)
        {
            auto& job = *runtime_->jobs_.find(output.job.id);
            if (!job.tryAdmit(context.frame_index, context.slot_index)) std::terminate();
            if (output.job.execution_generation != job.executionGeneration()) std::terminate();
            round.executions.push_back(job.execution.get());
        }
        for (std::size_t i = 0; i < inputs.storages.size(); ++i)
        {
            const auto& resource = inputs.storages[i];
            const auto use = inputs.used_storages[i];
            const bool consumes = use == FrameInputs::EResourceUse::INPUT_ONLY || use == FrameInputs::EResourceUse::GRAPH;
            if (use == FrameInputs::EResourceUse::GRAPH && resource.ops->prepareFrame)
                round.resource_uploads.emplace_back(static_cast<std::uint32_t>(i));
            if (consumes && resource.ops->selectRun)
                resource.ops->selectRun(resource.instance, round.replicaFrame());
        }
        if (round.outputs.empty())
        {
            // Only rejected input consumed this slot. Its PER_RUN rows have now been consumed as well.
            finishFrame(entry, {kJobState});
            return true;
        }
        if (round.error.type) return true;
        if (auto selected = round.selectConstants(); !selected) round.error = selected.error();
        else bindSelectedTargets(entry, round.slot());
        return true;
    }

    void RenderServer::Impl::finishFrame(detail::DeviceEntry& entry, RenderError error) noexcept
    {
        auto& round = entry.round;
        const auto slot = round.slot();
        if (round.active())
        {
            round.cancel(error);
        }
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto held_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - round.opened_at
        ).count();
        slot_cpu_hold_ns_.fetch_add(held_ns, std::memory_order_relaxed);
        const auto reads = round.submissions.takeRasterReads();
        raster_buffer_ranges_.fetch_add(reads.buffer_ranges, std::memory_order_relaxed);
        raster_buffer_bytes_.fetch_add(reads.buffer_bytes, std::memory_order_relaxed);
        raster_image_ranges_.fetch_add(reads.image_ranges, std::memory_order_relaxed);
        raster_image_subresources_.fetch_add(reads.image_subresources, std::memory_order_relaxed);
        raster_geometry_ranges_.fetch_add(reads.geometry_ranges, std::memory_order_relaxed);
        raster_geometry_bytes_.fetch_add(reads.geometry_bytes, std::memory_order_relaxed);
#endif
        const auto completed = round.executions.size();
        round.executions.clear();
        const auto failure = error.type ? error : RenderError{kJobState};
        for (auto& output : std::span{round.outputs}.first(completed))
        {
            auto& job = *runtime_->jobs_.find(output.job.id);
            auto& frame = job.execution->frame(slot);
            frame.target_reads.clear();
            frame.target_bindings.clear();
            ResourceRegistry::unpinBufferUses(job.execution->buffer_products);
            job.last_run_error = output.submitted ? RenderError{} : failure;
            job.finishRun(output.job.execution_generation, job.last_run_error);
            retireStoppedExecution(job);
        }
        round.outputs.clear();
        round.inputs.releaseTargetReads();
        round.error = {};
        round.stage = DeviceRound::EStage::IDLE;
    }

    void RenderServer::Impl::submitFrameRendering(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        round.pending = true;
        const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        stdexec::spawn(
            stdexec::starts_on(delivery, submitCurrentFrame(entry)) |
                stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                stdexec::upon_stopped(
                    []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kCommandCancelled}); }
                ) |
                stdexec::continues_on(delivery) |
                stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                stdexec::then(
                    [this, &entry](RenderResult<void> result) noexcept
                    {
                        auto& round = entry.round;
                        round.pending = false;
                        if (!result && !round.error.type) round.error = result.error();
                        if (round.error.type || !round.active()) finishFrame(entry, round.error);
                    }
                ),
            command_scope_.get_token()
        );
    }

    bool RenderServer::Impl::progressFrame(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        const bool opening = round.stage == DeviceRound::EStage::IDLE;
        if (opening)
        {
            if (stop_requested_.load() || entry.admissionError().type || !beginFrame(entry)) return false;
            if (!round.active() && !round.error.type) return true;
        }
        if (round.pending) return false;

        const auto ready = round.prepareNativeWork();
        if (!ready || round.error.type)
        {
            finishFrame(entry, round.error.type ? round.error : ready.error());
            return true;
        }
        if (*ready) submitFrameRendering(entry);
        return opening || *ready;
    }
} // namespace lux::render

namespace lux::render::runtime_detail
{
    using namespace detail;
    DeviceRound::DeviceRound(DeviceSession& device, FrameProgress& progress) noexcept
        : submissions(progress.frameCount(), device.queueCount()), device_(device), progress_(progress)
    {
    }

    DeviceRound::~DeviceRound() noexcept
    {
        if (advancing_ || pending) std::terminate();
        if (active()) cancel(RenderError{kCommandCancelled});
    }

    void DeviceRound::begin(FrameContext selected, ResourceUploader* uploads) noexcept
    {
        if (stage != EStage::IDLE || (uploads && &uploads->device_ != &device_)) std::terminate();
        progress_.beginFrame(selected);
        frame = selected;
        error = {};
        stage = EStage::RESOURCES;
        waiting_for_gpu_ = false;
        uploads_ = uploads;
        submission_.emplace(device_, progress_, frame, submissions);
        if (uploads_) uploads_->reservePreparationStaging(true);
    }

    void DeviceRound::finish() noexcept
    {
        for (auto* job : frozen_jobs_)
            job->frame(slot()).finishSubmission(job->graph(), device_);
        const bool unknown = error.type == kOutcomeUnknown;
        buffers_.finish(unknown);
        device_.descriptors().releaseFramePools(held_pools_);
        submission_.reset();
        frozen_jobs_.clear();
        buffer_uploads_.clear();
        next_upload_ = 0;
        progress_.endFrame(frame);
        stage = EStage::SETTLED;
        if (uploads_) uploads_->reservePreparationStaging(false);
        uploads_ = nullptr;
    }

    void DeviceRound::settleFrameCopies(FrameCopyReceipt receipt, RenderError failure) noexcept
    {
        if (settling_) std::terminate();
        settling_ = true;
        cxx::scope_exit settled([&]() noexcept { settling_ = false; });
        if (failure.type && uploads_) uploads_->cancelFrameCopies();
        buffers_.settle(receipt.point, failure);
        const auto acceptedPoint = [&](ReplicaCopyRange range) noexcept -> SubmissionPoint
        {
            if (failure.type || !range.count) return {};
            const bool outside = range.first > receipt.ranges || range.count > receipt.ranges - range.first;
            if (outside || !receipt.point.serial) std::terminate();
            return receipt.point;
        };
        const auto replica = replicaFrame();
        for (const auto& upload : std::span{resource_uploads}.first(attempted_uploads_))
        {
            const auto& source = inputs.storages[upload.slot];
            const auto point = acceptedPoint(upload.copies);
            if (source.ops->settleFrame)
                source.ops->settleFrame(source.instance, {replica, upload.copies, point, failure});
        }
        if (attempted_uploads_)
        {
            if (failure.type)
                resource_uploads.clear();
            else
                std::erase_if(resource_uploads, [](const ResourceUpload& upload) { return upload.ready; });
            attempted_uploads_ = 0;
        }
        // One staging window covers consecutive uploads and can end partway through its last buffer.
        while (next_upload_ < buffer_uploads_.size())
        {
            auto& upload = buffer_uploads_[next_upload_];
            if (!upload.ranges.count) break;
            upload.settle(acceptedPoint(upload.ranges), failure);
            if (upload.end != upload.bytes.size()) break;
            ++next_upload_;
        }
    }

    void DeviceRound::cancel(RenderError failure) noexcept
    {
        if (!active()) return;
        if (!error.type) error = failure;
        if (advancing_ || settling_) return;
        settleFrameCopies({}, error);
        submission_->rejectSuffix(error);
        finish();
    }

    RenderResult<bool> DeviceRound::prepareNativeWork() noexcept
    {
        if (error.type) return cxx::unexpected(error);
        if (const auto failure = device_.terminalError(); failure.type) return cxx::unexpected(failure);
        if (!active() || advancing_ || settling_) return cxx::unexpected(RenderError{kFrameContract});
        if (stage == EStage::SUBMITTING) return submission_->canSubmit();
        if (stage == EStage::FROZEN) return true;
        if (uploads_ && uploads_->hasFrameCopies()) return uploads_->canSubmitFrameCopies();
        if (waiting_for_gpu_ && device_.pendingGpuWork())
        {
            const auto& observed = blocked_completion_.serials();
            bool advanced = false;
            for (std::size_t i = 0; !advanced && i < observed.size(); ++i)
                advanced = device_.physicalQueue(i).completed() != observed[i];
            if (!advanced) return false;
        }

        // Resource callbacks may request cancellation. Keep their inputs alive until the owner call returns.
        advancing_ = true;
        cxx::scope_exit done([this]() noexcept { advancing_ = false; });
        if (stage == EStage::RESOURCES)
        {
            const auto resources = stageResourceUploads();
            if (!resources) return cxx::unexpected(resources.error());
            if (error.type) return cxx::unexpected(error);
            if (uploads_ && !uploads_->hasFrameCopies()) settleFrameCopies();
            if (error.type) return cxx::unexpected(error);
            if (*resources)
            {
                if (auto selected = selectFrameInputs(); !selected) return cxx::unexpected(selected.error());
                if (error.type) return cxx::unexpected(error);
                stage = EStage::FREEZE;
            }
        }

        auto waiting = EExecutionOutcome::RESOURCE_PREPARATION_PENDING;
        bool ready = false;
        if (stage == EStage::FREEZE)
        {
            const auto staged = stagePreparedBuffers();
            if (!staged) return cxx::unexpected(staged.error());
            if (*staged)
            {
                const auto frozen = freezeJobs();
                if (!frozen) return cxx::unexpected(frozen.error());
                ready = *frozen;
                waiting = EExecutionOutcome::HISTORY_PENDING;
            }
            else if (!(uploads_ && uploads_->hasFrameCopies()) && !device_.pendingGpuWork())
                return cxx::unexpected(RenderError{kDeviceCapacity});
        }
        if (error.type) return cxx::unexpected(error);
        // Drain a bounded staging window before reusing it. Its actual receipt supplies the graph's waits.
        if (uploads_ && uploads_->hasFrameCopies()) return uploads_->canSubmitFrameCopies();
        if (!ready)
        {
            if (!blocked_completion_.size()) blocked_completion_ = device_.makeFrontiers();
            device_.readCompleted(blocked_completion_);
            waiting_for_gpu_ = true;
            for (auto* job : executions) ++job->progress.counts[static_cast<std::size_t>(waiting)];
            return false;
        }

        waiting_for_gpu_ = false;
        settleFrameCopies();
        if (error.type) return cxx::unexpected(error);
        if (uploads_) uploads_->reservePreparationStaging(false);
        if (auto prepared = submission_->prepare(frozen_jobs_, job_order); !prepared)
            return cxx::unexpected(prepared.error());
        stage = EStage::FROZEN;
        return true;
    }

    exec::task<RenderResult<std::uint32_t>>
    DeviceRound::submitNativeWork(execution::CpuScheduler cpu, RecordingWorkers* workers) noexcept
    {
        if (!active() || advancing_ || settling_)
        {
            co_return cxx::unexpected(RenderError{kFrameContract});
        }
        advancing_ = true;
        cxx::scope_exit settle([this]() noexcept
        {
            advancing_ = false;
            if (error.type) cancel(error);
        });
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested()) error = RenderError{kCommandCancelled};
        if (const auto failure = device_.terminalError(); failure.type) error = failure;
        if (error.type) co_return cxx::unexpected(error);

        if (stage == EStage::RESOURCES || stage == EStage::FREEZE)
        {
            if (!uploads_ || !uploads_->hasFrameCopies()) co_return cxx::unexpected(RenderError{kFrameContract});
            const auto accepted = co_await uploads_->submitFrameCopies();
            if (!accepted)
            {
                error = accepted.error();
                co_return cxx::unexpected(error);
            }
            if (*accepted)
            {
                submission_->acceptPreparation((*accepted)->point);
                settleFrameCopies(**accepted);
            }
            if (error.type) co_return cxx::unexpected(error);
            co_return *accepted ? 1u : 0u;
        }
        if (stage == EStage::FROZEN)
        {
            RenderResult<void> recorded;
            if (!workers)
                recorded = co_await submission_->record(cpu, true);
            else if (workers->compilations_)
                recorded = co_await submission_->record(workers->independent_, false);
            else
                recorded = co_await workers->recordings_.nest(submission_->record(cpu, true));
            if (!recorded)
            {
                error = recorded.error();
                co_return cxx::unexpected(error);
            }
            stage = EStage::SUBMITTING;
        }
        auto accepted = co_await submission_->submit(error);
        if (!accepted) error = accepted.error();
        else if (submission_->complete()) finish();
        co_return accepted;
    }

    RenderResult<void> DeviceRound::selectFrameInputs() noexcept
    {
        // Shared views are owner-thread borrows. End them before staging can yield or the next mutation can run.
        cxx::scope_exit release_views([this]() noexcept { selected_views_.clear(); });
        std::size_t first_buffer = 0;
        for (std::size_t i = 0; i < executions.size(); ++i)
        {
            auto& job = *executions[i];
            const std::span<const std::uint32_t> slots = job.view_sources.empty() ? std::span<const std::uint32_t>{} :
                std::span<const std::uint32_t>{outputs[i].job.view_slots};
            if (!slots.empty() && selected_views_.empty()) selected_views_.resize(inputs.views.size());
            for (const auto index : slots)
            {
                auto& value = selected_views_[index];
                if (value.value) continue;
                const auto& source = inputs.views[index];
                auto selected = source.select(source.instance);
                if (!selected) return cxx::unexpected(selected.error());
                if (!selected->value) return cxx::unexpected(RenderError{kStorageContract});
                value = *selected;
            }
            const auto count = job.graph().preparedBufferCount();
            if (count > buffer_inputs_.max_size() - first_buffer) return cxx::unexpected(RenderError{kDeviceCapacity});
            const auto end = first_buffer + count;
            if (end > buffer_inputs_.size()) buffer_inputs_.resize(end);
            auto prepared = job.selectFrameInputs(
                slot(),
                std::span{buffer_inputs_}.subspan(first_buffer, count),
                {job.targetDescription(), selected_views_, slots},
                buffer_uploads_
            );
            if (!prepared) return prepared;
            first_buffer = end;
        }
        buffer_inputs_.resize(first_buffer);
        return {};
    }

    RenderResult<bool> DeviceRound::stagePreparedBuffers() noexcept
    {
        for (auto& upload : std::span{buffer_uploads_}.subspan(next_upload_))
        {
            if (!uploads_) return cxx::unexpected(RenderError{kDeviceUnsupported});
            if (upload.destination.generation != upload.generation) std::terminate();
            const auto first = uploads_->stagedFrameCopyCount();
            auto staged = uploads_->stageBufferPrefix({
                upload.destination.buffer, upload.begin, upload.bytes.subspan(upload.begin)
            });
            if (!staged) return cxx::unexpected(staged.error());
            upload.end = upload.begin + *staged;
            upload.ranges = {first, uploads_->stagedFrameCopyCount() - first};
            if (upload.end != upload.bytes.size()) return false;
        }
        return true;
    }

    RenderResult<void> DeviceRound::selectConstants() noexcept
    {
        for (auto* job : executions)
        {
            if (!job->device_graph) continue;
            auto& destination = job->frame_bindings.frames[slot()];
            for (const auto& block : job->device_graph->inline_blocks)
                destination.applyInlineConstant(job->graph(), block.pass, block.bytes, 0);
            for (const auto& input : job->device_graph->constant_buffers)
                if (auto selected = buffers_.select(input); !selected) return selected;
        }
        return {};
    }

    RenderResult<bool> DeviceRound::stageResourceUploads() noexcept
    {
        if (!resource_uploads.empty() && !uploads_) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto frame = replicaFrame();
        if (!buffers_.empty())
        {
            if (!uploads_) return cxx::unexpected(RenderError{kDeviceUnsupported});
            if (!blocked_completion_.size()) blocked_completion_ = device_.makeFrontiers();
            device_.readCompleted(blocked_completion_);
            const auto ready = buffers_.prepare(blocked_completion_, *uploads_);
            if (!ready || !*ready) return ready;
        }
        bool resources_ready = true;
        for (auto& upload : resource_uploads)
        {
            const auto& source = inputs.storages[upload.slot];
            ++attempted_uploads_;
            upload.copies.first = uploads_->stagedFrameCopyCount();
            const auto ready = source.ops->prepareFrame(source.instance, frame, *uploads_);
            upload.copies.count = uploads_->stagedFrameCopyCount() - upload.copies.first;
            if (!ready) return cxx::unexpected(ready.error());
            upload.ready = *ready;
            resources_ready &= upload.ready;
            if (error.type) return cxx::unexpected(error);
        }
        if (resources_ready && !buffers_.empty())
        {
            for (auto* job : executions)
            {
                if (!job->device_graph) continue;
                for (const auto& input : job->device_graph->constant_buffers)
                {
                    const auto binding = buffers_.binding(*input.storage);
                    job->frame_bindings.frames[frame.slot].bindResource(job->graph(), device_, input.resource.index, binding);
                }
            }
        }
        return resources_ready;
    }

    RenderResult<bool> DeviceRound::freezeJobs() noexcept
    {
        auto& frozen = frozen_jobs_;
        while (frozen.size() < executions.size())
        {
            auto& job = *executions[frozen.size()];
            const auto prepared = job.freezeFrame(slot(), freeze_draws_);
            if (!prepared)
            {
                return cxx::unexpected(prepared.error());
            }
            if (!*prepared)
            {
                return false;
            }
            device_.descriptors().retainFramePools(held_pools_);
            job.frame_bindings.frames[slot()].frozen_for_submission = true;
            frozen.push_back(&job);
        }
        return true;
    }
} // namespace lux::render::runtime_detail
