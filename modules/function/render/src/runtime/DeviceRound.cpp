#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    void
    RenderServer::Impl::queryFrameInputLayout(const RenderJobId& id, QueryRenderJobInputLayout::Response& output) noexcept
    {
        const auto* job = runtime_->jobs_.find(id);
        if (!job || !job->control.running())
        {
            output.error = {job ? kJobStopped : kJobIdentity};
            return;
        }
        output = {{}, job->execution->input_layout};
    }

    RenderResult<void> RenderServer::Impl::adoptFrameData(const JobExecution& execution, const FrameData& packet) noexcept
    {
        const auto layout = packet.layoutKey();
        if (!packet.sealed() || !layout.valid() || layout != execution.input_layout.key())
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        const auto has_updates = [&](const auto& input) noexcept
        {
            return std::ranges::any_of(input.columns, [&](auto column) noexcept
            {
                return packet.column(column).count != 0;
            });
        };
        for (const auto& input : execution.inputs)
        {
            if (!has_updates(input)) continue;
            if (runtime_->resources_.resourceInstance(input.identity) != input.object)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            if (runtime_->resources_.hasExclusiveCommand(input.identity))
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }
        }
        for (const auto& input : execution.inputs)
        {
            if (!has_updates(input)) continue;
            if (auto applied = input.call->invoke(input.object, packet, input.columns); !applied) return applied;
        }
        return {};
    }

    void RenderServer::Impl::updateFrameData(const UpdateFrameData::Request& input, AckResponse& output) noexcept
    {
        auto* job = runtime_->jobs_.find(input.job);
        if (!job) { output.error = {kJobIdentity}; return; }
        if (!job->control.running()) { output.error = {kJobStopped}; return; }
        if (!input.data) { output.error = {kFrameContract}; return; }
        const auto& execution = *job->execution;
        if (const auto error = runtime_->device(execution.device)->admissionError(); error.type)
        {
            output.error = error;
            return;
        }
        const auto updated = adoptFrameData(execution, *input.data);
        if (updated) job->last_run_error = {};
        output.error = updated ? RenderError{} : updated.error();
    }

    bool RenderServer::Impl::beginFrame(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        auto& progress = entry.frameProgress();
        const auto& inputs = round.inputs;
        const bool structure_changed = inputs.revision != job_structure_revision_;
        if (structure_changed) rebuildFrameInputs(entry);
        if (const auto selection = selectFrameInputs(entry, progress.nextSlot()); selection.type)
        {
            for (const auto& job : inputs.jobs) runtime_->jobs_.find(job.id)->control.rejectUnadmitted(selection);
            return false;
        }
        if (round.outputs.empty() || !progress.canBegin(entry.session())) return false;

        round.operation.emplace(
            entry.session(), entry.uploader(), progress, entry.frameWorkspace(), cpu_scheduler_, true, &entry
        );
        auto opened = round.operation->tryBegin();
        if (!opened || !*opened)
        {
            round.operation.reset();
            if (!opened)
            {
                for (const auto& output : round.outputs)
                    runtime_->jobs_.find(output.job)->control.rejectUnadmitted(opened.error());
            }
            return false;
        }
        const auto context = round.operation->context();
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        round.opened_at = std::chrono::steady_clock::now();
        rounds_opened_.fetch_add(1, std::memory_order_relaxed);
#endif
        round.stage = DeviceRound::EStage::ACQUIRING_OUTPUTS;
        for (auto& output : round.outputs)
        {
            output.submitted = false;
            auto& control = runtime_->jobs_.find(output.job)->control;
            if (!control.tryAdmit(context.frame_index, context.slot_index)) std::terminate();
            output.execution_generation = control.executionGeneration();
            ++round.run_count;
        }
        if (auto applied = selectGraphInputs(entry, round.slot()); !applied) round.error = applied.error();
        return true;
    }

    void RenderServer::Impl::finishFrame(detail::DeviceEntry& entry, RenderError error) noexcept
    {
        auto& round = entry.round;
        if (round.operation->active())
        {
            round.operation->cancel(error);
        }
        round.operation.reset();
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto held_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - round.opened_at
        ).count();
        slot_cpu_hold_ns_.fetch_add(held_ns, std::memory_order_relaxed);
        const auto reads = entry.frameWorkspace().submissions.takeRasterReads();
        raster_buffer_ranges_.fetch_add(reads.buffer_ranges, std::memory_order_relaxed);
        raster_buffer_bytes_.fetch_add(reads.buffer_bytes, std::memory_order_relaxed);
        raster_image_ranges_.fetch_add(reads.image_ranges, std::memory_order_relaxed);
        raster_image_subresources_.fetch_add(reads.image_subresources, std::memory_order_relaxed);
        raster_geometry_ranges_.fetch_add(reads.geometry_ranges, std::memory_order_relaxed);
        raster_geometry_bytes_.fetch_add(reads.geometry_bytes, std::memory_order_relaxed);
#endif
        round.error = {};
        const auto completed = std::exchange(round.run_count, 0);
        const auto failure = error.type ? error : RenderError{kJobState};
        for (auto& output : std::span{round.outputs}.first(completed))
        {
            auto& job = *runtime_->jobs_.find(output.job);
            job.last_run_error = output.submitted ? RenderError{} : failure;
            output.completion = job.control.finishRun(
                output.execution_generation, output.submitted ? RenderResult<void>{} : cxx::unexpected(failure)
            );
            retireStoppedExecution(job);
        }
        for (auto& output : std::span{round.outputs}.first(completed))
        {
            if (auto completion = std::exchange(output.completion, std::nullopt))
                std::move(*completion).deliver();
        }
    }

    bool RenderServer::Impl::progressProgramPreparations(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        if (round.operation || stop_requested_.load() || entry.admissionError().type) return false;

        bool progressed = false;
        for (auto id : running_jobs_)
        {
            auto& job = *runtime_->jobs_.find(id);
            if (!job.control.eligible()) continue;
            auto& execution = *job.execution;
            if (execution.device != entry.id) continue;

            if (const auto error = execution.programError(); error.type && job.control.hasRequest())
            {
                job.control.rejectUnadmitted(error);
                progressed = true;
            }
        }
        if (preparing_execution_)
        {
            auto& programs = preparing_execution_->programs;
            if (!preparing_execution_->instance.matchesProgramRevisions(programs.revisions))
                programs.stop->request_stop();
            return progressed;
        }
        const auto count = running_jobs_.size();
        for (std::size_t offset = 0; offset < count; ++offset)
        {
            const auto index = (round.preparation_cursor + offset) % count;
            auto& job = *runtime_->jobs_.find(running_jobs_[index]);
            if (!job.readyToRun() || job.control.pendingStart() || !jobTargetReady(job)) continue;
            auto& execution = *job.execution;
            if (execution.device != entry.id) continue;
            if (execution.programsReady() || execution.programError().type) continue;
            if (execution.pending_uses || !resourcesAvailable(execution.retainedResources())) continue;
            const bool parallel_recording = std::ranges::any_of(runtime_->devices_, [](const auto& device) noexcept
            {
                const auto& operation = device->round.operation;
                return operation && operation->parallelRecordingPending();
            });
            if (parallel_recording)
            {
                program_waiting_for_recording_ = true;
                return progressed;
            }
            round.preparation_cursor = (index + 1) % count;
            startProgramPreparation(job, execution);
            return true;
        }
        return progressed;
    }

    void RenderServer::Impl::startProgramPreparation(RenderJob& job, JobExecution& execution) noexcept
    {
        auto& programs = execution.programs;
        execution.instance.captureProgramRevisions(programs.revisions);
        programs.failure = {};
        programs.stop.emplace();
        preparing_execution_ = &execution;
        preparation_targets_.clear();
        preparation_targets_.push_back(execution.target);
        for (const auto& input : execution.target_images) preparation_targets_.push_back(input.target);
        const auto stop = programs.stop->get_token();
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        program_preparation_started_at_ = std::chrono::steady_clock::now();
        program_preparations_.fetch_add(1, std::memory_order_relaxed);
#endif
        const auto scheduler = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        auto prepared =
            execution.instance.prepareProducerPrograms(program_scheduler_, programs.revisions, stop) |
            stdexec::upon_error([](std::exception_ptr) noexcept -> RenderResult<bool> { std::terminate(); }) |
            stdexec::upon_stopped([]() noexcept -> RenderResult<bool> { return false; }) |
            stdexec::continues_on(scheduler) |
            stdexec::upon_error([](cxx::execution::ScheduleError) noexcept -> RenderResult<bool> { std::terminate(); }) |
            stdexec::then(
                [this, id = job.control.id(), generation = job.control.executionGeneration()](RenderResult<bool> result)
                    noexcept
                {
                    auto* job = runtime_->jobs_.find(id);
                    if (!job || job->control.executionGeneration() != generation ||
                        job->execution.get() != preparing_execution_) std::terminate();
                    auto& execution = *job->execution;
                    auto& programs = execution.programs;
                    const bool discarded = programs.stop->stop_requested() ||
                        !execution.instance.matchesProgramRevisions(programs.revisions);
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
                    const auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - program_preparation_started_at_
                    ).count();
                    program_preparation_ns_.fetch_add(duration_ns, std::memory_order_relaxed);
                    if (discarded) discarded_preparations_.fetch_add(1, std::memory_order_relaxed);
#endif
                    if (!discarded)
                    {
                        if (!result) programs.failure = result.error();
                        else if (!*result || execution.instance.requiresProducerPrograms()) programs.failure = {kFrameContract};
                    }
                    programs.stop.reset();
                    preparing_execution_ = nullptr;
                    preparation_targets_.clear();
                    retireStoppedExecution(*job);
                }
            );
        stdexec::spawn(
            std::move(prepared),
            command_scope_.get_token(),
            stdexec::env{stdexec::prop{stdexec::get_scheduler, scheduler}}
        );
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
                        if (round.error.type || !round.operation->active()) finishFrame(entry, round.error);
                    }
                ),
            command_scope_.get_token()
        );
    }

    bool RenderServer::Impl::progressFrame(detail::DeviceEntry& entry) noexcept
    {
        auto& round = entry.round;
        if (!round.operation)
        {
            return !stop_requested_.load() && !entry.admissionError().type && beginFrame(entry);
        }
        if (round.pending) return false;
        if (round.error.type)
        {
            finishFrame(entry, round.error);
            return true;
        }
        if (round.stage == DeviceRound::EStage::ACQUIRING_OUTPUTS)
        {
            const auto acquired = acquireFrameOutputs(entry, round.slot());
            if (!acquired)
            {
                finishFrame(entry, acquired.error());
                return true;
            }
            if (!*acquired) return false;
            round.stage = DeviceRound::EStage::SUBMITTING;
        }
        if (round.stage == DeviceRound::EStage::SUBMITTING && round.operation->canRun())
        {
            submitFrameRendering(entry);
            return true;
        }
        return false;
    }
} // namespace lux::render
