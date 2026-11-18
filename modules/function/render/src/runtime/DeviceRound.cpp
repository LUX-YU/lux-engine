#include <exec/create.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/resource/view/CameraInputHandler.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    void
    RenderServer::Impl::queryFrameInputLayout(const RenderJobId& id, QueryRenderJobInputLayout::Response& output) noexcept
    {
        const auto* job = domain_->jobs_.find(id);
        if (!job || !job->control.running())
        {
            output.error = {job ? kJobStopped : kJobIdentity};
            return;
        }
        output = {{}, job->execution->input_layout};
    }

    void RenderServer::Impl::updateViewCamera(const UpdateViewCamera::Request& input, AckResponse& output) noexcept
    {
        auto* scene = domain_->scenes_.find(input.view.scene);
        auto* view = scene ? scene->views.find(input.view.view) : nullptr;
        if (!view) { output.error = {kRuntimeIdentity}; return; }
        const auto applied = detail::CameraInputHandler::apply(view->camera, view->camera_revision, input.camera);
        output.error = applied ? RenderError{} : applied.error();
    }

    RenderResult<void> RenderServer::Impl::applyFrameData(const JobExecution& execution, const FrameData& packet) noexcept
    {
        if (!packet.sealed() || !packet.layout().hasSameIdentity(execution.input_layout))
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
            if (domain_->resources_.resourceInstance(input.identity) != input.object)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            if (domain_->resources_.hasExclusiveCommand(input.identity))
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

    exec::task<AckResponse> RenderServer::Impl::updateFrameData(UpdateFrameData::Request&& input) noexcept
    {
        auto* job = domain_->jobs_.find(input.job);
        if (!job) co_return AckResponse{RenderError{kJobIdentity}};
        if (!job->control.running()) co_return AckResponse{RenderError{kJobStopped}};
        if (!input.data) co_return AckResponse{RenderError{kFrameContract}};
        auto& execution = *job->execution;
        if (!round_.operation && input_updates_.empty())
        {
            const auto updated = applyFrameData(execution, *input.data);
            if (updated && job->last_run_error.type)
            {
                job->last_run_error = {};
                active_frame_inputs_dirty_ = true;
            }
            co_return AckResponse{updated ? RenderError{} : updated.error()};
        }
        if (execution.pending_uses == UINT32_MAX) co_return AckResponse{RenderError{kRuntimeCapacity}};
        InputUpdate pending{input.job, input.data, &execution, nullptr, nullptr};
        co_return co_await exec::create<stdexec::set_value_t(AckResponse)>([this, &pending](auto& context) noexcept
        {
            using Context = std::remove_reference_t<decltype(context)>;
            ++pending.execution->pending_uses;
            pending.receiver = &context;
            pending.complete = [](void* receiver, AckResponse response) noexcept
            {
                auto& context = *static_cast<Context*>(receiver);
                stdexec::set_value(std::move(context.receiver), response);
            };
            input_updates_.push_back(&pending);
        });
    }

    bool RenderServer::Impl::progressInputUpdates() noexcept
    {
        if (round_.operation || input_updates_.empty()) return false;
        const auto count = std::min<std::size_t>(input_updates_.size(), 32);
        for (std::size_t i = 0; i < count; ++i)
        {
            auto& update = *input_updates_.front();
            input_updates_.pop_front();
            auto& job = *domain_->jobs_.find(update.job);
            RenderError error;
            if (stop_requested_.load()) error = {kCommandCancelled};
            else if (!job.control.running()) error = {kJobStopped};
            else if (auto result = applyFrameData(*update.execution, *update.data); !result) error = result.error();
            if (!error.type && job.last_run_error.type)
            {
                job.last_run_error = {};
                active_frame_inputs_dirty_ = true;
            }
            --update.execution->pending_uses;
            retireStoppedExecution(job);
            update.complete(update.receiver, {error});
        }
        return true;
    }

    bool RenderServer::Impl::beginFrame() noexcept
    {
        auto& entry = *domain_->device_;
        auto& progress = entry.frameProgress();
        prefetchFrameOutputs(progress.nextSlot());
        if (!active_frame_inputs_dirty_)
        {
            active_frame_inputs_dirty_ = std::ranges::any_of(pending_history_outputs_, [&](const auto& output)
            {
                const auto [target, slot] = output;
                return publishedOutputSlot(target) != slot || publishedOutputReadable(target, slot);
            });
        }
        if (active_frame_inputs_dirty_ || output_uses_revision_ != domain_->resources_.outputUsesRevision())
        {
            rebuildActiveFrameInputs();
        }
        if (frame_input_error_.type)
        {
            for (auto id : running_jobs_) domain_->jobs_.find(id)->control.rejectUnadmitted(frame_input_error_);
            active_frame_inputs_dirty_ = true;
            return false;
        }
        if (frame_outputs_.empty() || !progress.canBegin(entry.session())) return false;

        // A parallel bulk sender joins tasks assigned to every worker. While one worker may
        // be inside a driver call, record on the other worker with the same frozen batch path.
        const bool parallel_recording = !preparing_execution_;
        const auto cpu = parallel_recording ? cpu_scheduler_ : recording_scheduler_;
        round_.operation.emplace(
            entry.session(), entry.uploader(), progress, entry.frameWorkspace(), cpu, parallel_recording
        );
        auto opened = round_.operation->tryBegin();
        if (!opened || !*opened)
        {
            round_.operation.reset();
            if (!opened)
            {
                for (const auto& output : frame_outputs_)
                    domain_->jobs_.find(output.job)->control.rejectUnadmitted(opened.error());
                active_frame_inputs_dirty_ = true;
            }
            return false;
        }
        const auto context = round_.operation->context();
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        slot_opened_at_ = std::chrono::steady_clock::now();
        rounds_opened_.fetch_add(1, std::memory_order_relaxed);
#endif
        round_.stage = DeviceRound::EStage::ACQUIRING_OUTPUTS;
        if (frame_runs_.size() < frame_outputs_.size()) frame_runs_.resize(frame_outputs_.size());
        for (auto& output : frame_outputs_)
        {
            output.submitted = false;
            auto& control = domain_->jobs_.find(output.job)->control;
            if (!control.tryAdmit(context.frame_index, context.slot_index, frame_runs_[frame_run_count_++]))
                std::terminate();
        }
        if (auto applied = applyParameterSources(round_.slot()); !applied) round_.error = applied.error();
        return true;
    }

    void RenderServer::Impl::finishFrame(RenderError error) noexcept
    {
        if (round_.operation->active())
        {
            round_.operation->cancel(error);
        }
        round_.operation.reset();
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto held_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - slot_opened_at_
        ).count();
        slot_cpu_hold_ns_.fetch_add(held_ns, std::memory_order_relaxed);
        const auto reads = domain_->device_->frameWorkspace().submissions.takeRasterReads();
        raster_buffer_ranges_.fetch_add(reads.buffer_ranges, std::memory_order_relaxed);
        raster_buffer_bytes_.fetch_add(reads.buffer_bytes, std::memory_order_relaxed);
        raster_image_ranges_.fetch_add(reads.image_ranges, std::memory_order_relaxed);
        raster_image_subresources_.fetch_add(reads.image_subresources, std::memory_order_relaxed);
#endif
        round_.error = {};
        for (std::size_t i = 0; i < frame_run_count_; ++i)
        {
            const auto& output = frame_outputs_[i];
            auto& job = *domain_->jobs_.find(output.job);
            const auto failure = error.type ? error : RenderError{kJobState};
            job.last_run_error = output.submitted ? RenderError{} : failure;
            frame_runs_[i].finish(output.submitted ? RenderResult<void>{} : cxx::unexpected(failure));
            if (job.last_run_error.type || job.control.cadence() == EJobCadence::REQUESTED)
                active_frame_inputs_dirty_ = true;
            retireStoppedExecution(job);
        }
        frame_run_count_ = 0;
    }

    bool RenderServer::Impl::progressProgramPreparations() noexcept
    {
        if (round_.operation || stop_requested_.load()) return false;

        bool progressed = false;
        // Settle every newly accepted packet before capturing any program revision.
        // JobControl keeps the request until its one terminal response.
        for (auto id : running_jobs_)
        {
            auto& job = *domain_->jobs_.find(id);
            if (!job.control.eligible()) continue;
            auto& execution = *job.execution;
            if (const auto* packet = job.control.takeRequestedData())
            {
                const auto updated = applyFrameData(execution, *packet);
                job.last_run_error = updated ? RenderError{} : updated.error();
                if (!updated) job.control.rejectUnadmitted(updated.error());
                active_frame_inputs_dirty_ = true;
                progressed = true;
            }
            if (const auto error = execution.programError(); error.type && job.control.hasRequest())
            {
                job.control.rejectUnadmitted(error);
                active_frame_inputs_dirty_ = true;
                progressed = true;
            }
        }
        if (preparing_execution_)
        {
            auto& programs = preparing_execution_->programs;
            if (!preparing_execution_->instance.matchesProgramRevisions(programs.revisions))
                programs.active->stop.request_stop();
            return progressed;
        }
        const auto count = running_jobs_.size();
        for (std::size_t offset = 0; offset < count; ++offset)
        {
            const auto index = (preparation_cursor_ + offset) % count;
            auto& job = *domain_->jobs_.find(running_jobs_[index]);
            if (!job.readyToRun() || job.control.pendingStart() || !jobTargetReady(job)) continue;
            auto& execution = *job.execution;
            if (execution.programsReady() || execution.programError().type) continue;
            if (execution.pending_uses || !resourcesAvailable(execution.resources)) continue;
            preparation_cursor_ = (index + 1) % count;
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
        if (execution.pending_uses == UINT32_MAX)
        {
            programs.failure = {kRuntimeCapacity};
            return;
        }
        programs.active.emplace(execution);
        preparing_execution_ = &execution;
        preparation_targets_ = execution.instance.sampledTargets();
        preparation_targets_.push_back(execution.target);
        for (const auto& input : execution.target_images) preparation_targets_.push_back(input.target);
        const auto stop = programs.active->stop.get_token();
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        program_preparation_started_at_ = std::chrono::steady_clock::now();
        program_preparations_.fetch_add(1, std::memory_order_relaxed);
#endif
        const auto scheduler = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        auto prepared =
            execution.instance.prepareDataPrograms(program_scheduler_, programs.revisions, stop) |
            stdexec::upon_error([](std::exception_ptr) noexcept -> RenderResult<bool> { std::terminate(); }) |
            stdexec::upon_stopped([]() noexcept -> RenderResult<bool> { return false; }) |
            stdexec::continues_on(scheduler) |
            stdexec::upon_error([](cxx::execution::ScheduleError) noexcept -> RenderResult<bool> { std::terminate(); }) |
            stdexec::then(
                [this, &job, &execution](RenderResult<bool> result) noexcept
                {
                    auto& programs = execution.programs;
                    const bool discarded = programs.active->stop.stop_requested() ||
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
                        else if (!*result || execution.instance.requiresDataPrograms()) programs.failure = {kFrameContract};
                    }
                    programs.active.reset();
                    preparing_execution_ = nullptr;
                    preparation_targets_.clear();
                    active_frame_inputs_dirty_ = true;
                    retireStoppedExecution(job);
                }
            );
        stdexec::spawn(
            std::move(prepared),
            command_scope_.get_token(),
            stdexec::env{stdexec::prop{stdexec::get_scheduler, scheduler}}
        );
    }

    void RenderServer::Impl::submitFrameRendering() noexcept
    {
        round_.pending = true;
        const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        stdexec::spawn(
            stdexec::starts_on(delivery, submitCurrentFrame()) |
                stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                stdexec::upon_stopped(
                    []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kCommandCancelled}); }
                ) |
                stdexec::continues_on(delivery) |
                stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                stdexec::then(
                    [this](RenderResult<void> result) noexcept
                    {
                        round_.pending = false;
                        if (!result) round_.error = result.error();
                        if (round_.error.type || !round_.operation->active()) finishFrame(round_.error);
                    }
                ),
            command_scope_.get_token()
        );
    }

    bool RenderServer::Impl::progressFrame() noexcept
    {
        if (!round_.operation)
        {
            return !stop_requested_.load() && beginFrame();
        }
        if (round_.pending) return false;
        if (round_.error.type)
        {
            finishFrame(round_.error);
            return true;
        }
        if (round_.stage == DeviceRound::EStage::ACQUIRING_OUTPUTS)
        {
            const auto acquired = acquireFrameOutputs(round_.slot());
            if (!acquired)
            {
                finishFrame(acquired.error());
                return true;
            }
            if (!*acquired) return false;
            round_.stage = DeviceRound::EStage::SUBMITTING;
        }
        if (round_.stage == DeviceRound::EStage::SUBMITTING && round_.operation->canRun())
        {
            submitFrameRendering();
            return true;
        }
        return false;
    }
} // namespace lux::render
