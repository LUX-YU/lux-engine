#include <lux/engine/render/features/common/ParameterLayout.hpp>
#include <lux/engine/render/resource/view/CameraParameters.hpp>
#include <exception>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <ranges>

namespace lux::render
{
    RenderServer::Impl::Impl(RenderServerConfig value, ipc::IpcResponder input, execution::ExecutionRuntime& execution)
        : config_(std::move(value)), cpu_scheduler_(execution.cpuScheduler()),
          program_scheduler_(execution.cpuSchedulerOnWorker(0)),
          recording_scheduler_(execution.cpuSchedulerOnWorker(execution.cpuConcurrency() > 1 ? 1 : 0)),
          responder_(std::move(input)),
          server_context_(
              cxx::execution::default_manual_context_config(),
              {nullptr, [](void*) noexcept { ipc::IpcCore::instance().notifyWork(); }}
          )
    {
        const auto query = bindFrameworkCommand<QueryCommandIds, &Impl::queryCommandIds>();
        if (query != QueryCommandIds::id) std::terminate(); // Fixed bootstrap identity is a protocol invariant.
        bindFrameworkCommand<QueryResource, &Impl::queryResource>();
        bindFrameworkCommand<GetOrCreateResource, &Impl::getOrCreateResource>();
        bindFrameworkCommand<CreateScene, &Impl::createScene>();
        bindFrameworkCommand<RemoveScene, &Impl::removeScene>();
        bindFrameworkCommand<CreateView, &Impl::createView>();
        bindFrameworkCommand<UpdateViewCamera, &Impl::updateViewCamera>();
        bindFrameworkCommand<QueryRenderJobInputLayout, &Impl::queryFrameInputLayout>();
        bindFrameworkCommand<UpdateFrameData, &Impl::updateFrameData>();
        bindFrameworkCommand<RemoveView, &Impl::removeView>();
        bindFrameworkCommand<CreateTarget, &Impl::createTarget>();
        bindFrameworkCommand<ResizeTarget, &Impl::resizeTarget>();
        bindFrameworkCommand<RemoveTarget, &Impl::removeTarget>();
        bindFrameworkCommand<SetTargetActive, &Impl::setTargetActive>();
        bindCreateEffectCommand();
        bindFrameworkCommand<RemoveEffect, &Impl::removeEffect>();
        bindFrameworkCommand<SetEffectParameters, &Impl::setEffectParameters>();
        bindJobCommands();
        bindFrameworkCommand<Stop, &Impl::stopCommand>();
        bindFrameworkCommand<QueryRenderStats, &Impl::queryRenderStats>();
        bindFrameworkCommand<CreateDeviceSession, &Impl::createDeviceSession>();
        bindFrameworkCommand<RemoveDeviceSession, &Impl::removeDeviceSession>();
        bindFrameworkCommand<QueryRenderDevice, &Impl::queryRenderDevice>();
        bindFrameworkCommand<QueryExecutionProgress, &Impl::queryExecutionProgress>();
        bindFrameworkCommand<ListCapabilities, &Impl::listCapabilities>();
        bindFrameworkCommand<ListCommands, &Impl::listCommands>();
        runtime_->resources_.setOutputResolver(this, [](void* owner, const detail::DeviceSession& device, TargetId id, std::uint32_t slot) noexcept
            -> RenderResult<ResourceRegistry::SampledImage>
        {
            auto& self = *static_cast<Impl*>(owner);
            const auto* target = self.runtime_->targets_.find(id);
            if (!target || !targetDescription(*target).sampled || targetState(*target).resizing)
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (!targetBelongsTo(*target, device)) return cxx::unexpected(RenderError{kJobState});
            const auto selected = self.outputSlot(id, slot);
            if (selected == UINT32_MAX) return cxx::unexpected(RenderError{kRuntimeState});
            const auto frame = targetOutput(*target).frames[selected];
            return ResourceRegistry::SampledImage{frame.image, frame.sampled_index};
        });
    }

    void NRI_CALL RenderServer::Impl::diagnostic(
        nri::Message severity,
        const char* file,
        std::uint32_t line,
        const char* message,
        void* user
    ) noexcept
    {
        auto& state = *static_cast<Impl*>(user);
        if (severity == nri::Message::ERROR)
        {
            state.validation_errors_.fetch_add(1, std::memory_order_relaxed);
        }
        if (severity == nri::Message::WARNING)
        {
            state.validation_warnings_.fetch_add(1, std::memory_order_relaxed);
        }
        const auto& sink = state.config_.diagnostics;
        if (sink.report)
        {
            const auto level = severity == nri::Message::ERROR     ? ERenderDiagnosticSeverity::ERROR_MESSAGE
                               : severity == nri::Message::WARNING ? ERenderDiagnosticSeverity::WARNING
                                                                   : ERenderDiagnosticSeverity::INFORMATION;
            sink.report(
                sink.context,
                {level,
                 RenderError{kDeviceDiagnostic, {static_cast<std::uint64_t>(level), line}},
                 message ? std::string_view(message) : std::string_view{},
                 file ? std::string_view(file) : std::string_view{},
                 line}
            );
        }
    }

    bool RenderServer::Impl::targetDependencyCycle(
        TargetId destination,
        std::span<const TargetImageInput> inputs,
        RenderJobId replaced
    ) const noexcept
    {
        std::vector<TargetId> pending, visited;
        for (const auto& input : inputs)
        {
            pending.push_back(input.target);
        }
        while (!pending.empty())
        {
            const auto id = pending.back();
            pending.pop_back();
            if (id == destination)
            {
                return true;
            }
            if (std::ranges::find(visited, id) != visited.end())
            {
                continue;
            }
            visited.push_back(id);
            const auto* target = runtime_->targets_.find(id);
            const auto writer = target ? targetState(*target).writer : RenderJobId{};
            const auto* job = writer != replaced ? runtime_->jobs_.find(writer) : nullptr;
            if (job)
            {
                for (const auto& input : job->execution->target_images)
                {
                    pending.push_back(input.target);
                }
            }
        }
        return false;
    }

    void RenderServer::Impl::createTarget(const CreateTarget::Request& input, CreateTarget::Response& response) noexcept
    {
        auto* entry = runtime_->device(input.device);
        if (!entry)
        {
            response.error = {kRuntimeIdentity};
            return;
        }
        if (auto valid = input.description.validate(); !valid)
        {
            response.error = valid.error();
            return;
        }
        const bool is_surface = input.description.kind == ETargetKind::SURFACE;
        if (is_surface != input.surface.has_value())
        {
            response.error = RenderError{kCommandContract};
            return;
        }
        TargetId id;
        if (is_surface)
        {
            auto physical = detail::SwapchainTarget::create(
                entry->session(), input.description, *input.surface, entry->frameProgress().frameCount()
            );
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            id = runtime_->targets_.emplace(std::move(*physical));
        }
        else
        {
            auto physical = detail::OffscreenTarget::create(
                entry->session(), input.description, entry->frameProgress().frameCount()
            );
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            id = runtime_->targets_.emplace(std::move(*physical));
        }
        targetState(*runtime_->targets_.find(id)).device = input.device;
        response = TargetCreated{{}, id, 1, targetDescription(*runtime_->targets_.find(id)), true};
    }

    RenderResult<RenderServer::Impl::ResizeCandidates> RenderServer::Impl::prepareTargetResize(
        TargetId target,
        detail::TargetOutput output,
        std::span<const RenderJobId> affected
    ) noexcept
    {
        ResizeCandidates candidates;
        candidates.reserve(affected.size());
        for (auto id : affected)
        {
            auto& execution = *runtime_->jobs_.find(id)->execution;
            std::vector<detail::RenderGraphInstance::ImageReplacement> inputs;
            for (const auto& image : execution.target_images)
            {
                if (image.target == target)
                {
                    inputs.push_back({image.resource, output.frames});
                }
            }
            auto changed = execution.instance.prepareResize(
                execution.target == target ? output : targetOutput(*runtime_->targets_.find(execution.target)),
                inputs
            );
            if (!changed)
            {
                return cxx::unexpected(changed.error());
            }
            candidates.emplace_back(id, std::move(*changed));
        }
        return candidates;
    }

    std::vector<RenderJobId> RenderServer::Impl::jobsSamplingTarget(TargetId source) const noexcept
    {
        std::vector<RenderJobId> result;
        for (const auto& job : runtime_->jobs_)
        {
            const auto* execution = job.execution.get();
            if (!execution) continue;
            const bool binds_source = std::ranges::find(
                execution->target_images, source, &JobExecution::TargetImage::target
            ) != execution->target_images.end();
            const auto* target = runtime_->targets_.find(source);
            if (!target || targetState(*target).device != execution->device) continue;
            const auto sampled = execution->instance.sampledTargets();
            if (binds_source || std::ranges::find(sampled, source) != sampled.end())
                result.push_back(job.control.id());
        }
        return result;
    }

    exec::task<TargetCreated> RenderServer::Impl::resizeTarget(ResizeTarget::Request&& input) noexcept
    {
        auto* target = runtime_->targets_.find(input.target);
        if (!target) co_return TargetCreated{RenderError{kRuntimeIdentity}};
        const auto response = [&](RenderError error = {}) noexcept
        {
            return TargetCreated{error, input.target, targetState(*target).epoch, targetDescription(*target),
                                 targetAvailable(*target)};
        };
        if (auto valid = input.description.validate(); !valid) co_return response(valid.error());

        auto& state = targetState(*target);
        const bool busy_target = state.resizing || state.reservation ||
            frameUsesTarget(input.target) || preparationUsesTarget(input.target);
        if (busy_target) co_return response(RenderError{kDeviceBusy});
        auto affected = jobsSamplingTarget(input.target);
        const bool different_kind = input.description.kind != targetDescription(*target).kind;
        auto& device = *runtime_->device(state.device);
        const auto maximum = device.session().description().dimensions.attachmentMaxDim;
        const bool oversized = input.description.width > maximum || input.description.height > maximum;
        const bool removes_sampling = !input.description.sampled && !affected.empty();
        if (different_kind || oversized || removes_sampling) co_return response(RenderError{kCommandContract});
        auto* swapchain = std::get_if<detail::SwapchainTarget>(target);
        const bool healthy = !swapchain || !swapchain->needsRecreation();
        if (input.description == targetDescription(*target) && healthy) co_return response();
        if (!state.writer.isNull())
        {
            affected.push_back(state.writer);
            if (!runtime_->jobs_.find(state.writer)->execution->plan.compatible(input.description))
                co_return response(RenderError{kDeviceArgument});
        }
        const bool busy_execution = std::ranges::any_of(affected, [&](auto id)
        {
            return !canChangeExecution(*runtime_->jobs_.find(id));
        });
        if (busy_execution) co_return response(RenderError{kDeviceBusy});
        if (state.epoch == UINT64_MAX) co_return response(RenderError{kRuntimeCapacity});

        const auto writer_at_admission = state.writer;
        std::vector<detail::RenderGraphInstance*> instances;
        for (auto id : affected)
        {
            auto& execution = *runtime_->jobs_.find(id)->execution;
            ++execution.pending_uses;
            instances.push_back(&execution.instance);
        }
        state.resizing = true;
        cxx::scope_exit finish([&]() noexcept
        {
            targetState(*target).resizing = false;
            for (auto id : affected)
            {
                auto& job = *runtime_->jobs_.find(id);
                --job.execution->pending_uses;
                retireStoppedExecution(job);
            }
        });

        auto accepted = device.frameWorkspace().submissions.acceptedUses(
            device.session(), device.frameProgress(), instances
        );
        std::optional<VRenderTarget> candidate_target;
        ResizeCandidates candidates;
        if (auto* offscreen = std::get_if<detail::OffscreenTarget>(target))
        {
            offscreen->appendUses(accepted);
            auto created = detail::OffscreenTarget::create(device.session(), input.description, device.frameProgress().frameCount());
            if (!created) co_return response(created.error());
            candidate_target.emplace(std::move(*created));
            auto prepared = prepareTargetResize(input.target, targetOutput(*candidate_target), affected);
            if (!prepared) co_return response(prepared.error());
            candidates = std::move(*prepared);
        }
        if (auto waited = co_await device.session().waitFrontiers(accepted); !waited) co_return response(waited.error());
        if (stop_requested_.load()) co_return response(RenderError{kCommandCancelled});
        if (swapchain)
        {
            const auto window = swapchain->surface();
            if (!writer_at_admission.isNull())
            {
                auto& writer = *runtime_->jobs_.find(writer_at_admission);
                writer.execution->instance.releaseOutputReferences();
                writer.control.rejectUnadmitted(RenderError{kJobStopped});
            }
            state.enabled = false;
            auto closed = co_await swapchain->close();
            if (!closed) co_return response(closed.error());
            if (stop_requested_.load()) co_return response(RenderError{kCommandCancelled});
            auto created = detail::SwapchainTarget::create(device.session(), input.description, window, device.frameProgress().frameCount());
            if (!created) co_return response(created.error());
            candidate_target.emplace(std::move(*created));
            auto prepared = prepareTargetResize(input.target, targetOutput(*candidate_target), affected);
            if (!prepared) co_return response(prepared.error());
            candidates = std::move(*prepared);
        }
        for (auto& [id, candidate] : candidates)
        {
            if (auto ready = runtime_->jobs_.find(id)->execution->instance.prepareResizeAdoption(candidate); !ready)
                co_return response(ready.error());
        }
        for (auto& [id, candidate] : candidates)
            runtime_->jobs_.find(id)->execution->instance.commitResize(std::move(candidate));
        auto next = state;
        ++next.epoch;
        next.enabled = false;
        if (!next.writer.isNull())
            runtime_->jobs_.find(next.writer)->control.rejectUnadmitted(RenderError{kJobStopped});
        std::visit([&](auto& value) noexcept
        {
            using T = std::remove_cvref_t<decltype(value)>;
            value.state = next;
            target->emplace<T>(std::move(value));
        }, *candidate_target);
        runtime_->job_outputs_.erase(input.target);
        co_return response();
    }

    exec::task<AckResponse> RenderServer::Impl::removeTarget(RemoveTarget::Request&& input) noexcept
    {
        auto* target = runtime_->targets_.find(input);
        if (!target) co_return AckResponse{RenderError{kRuntimeIdentity}};
        auto& state = targetState(*target);
        const bool busy_target = state.resizing || state.reservation ||
            frameUsesTarget(input) || preparationUsesTarget(input);
        if (busy_target) co_return AckResponse{RenderError{kDeviceBusy}};
        if (!jobsSamplingTarget(input).empty()) co_return AckResponse{RenderError{kRuntimeState}};
        if (!state.writer.isNull())
        {
            if (!canChangeExecution(*runtime_->jobs_.find(state.writer))) co_return AckResponse{RenderError{kDeviceBusy}};
            stopJob(*runtime_->jobs_.find(state.writer));
        }
        state.resizing = true;
        RenderError retired;
        if (auto* surface = std::get_if<detail::SwapchainTarget>(target))
        {
            if (auto closed = co_await surface->close(); !closed)
            {
                retired = closed.error();
                failExecution(retired);
            }
        }
        runtime_->job_outputs_.erase(input);
        runtime_->targets_.erase(input);
        co_return AckResponse{retired};
    }

    void RenderServer::Impl::setTargetActive(
        const SetTargetActive::Request& input,
        SetTargetActive::Response& response
    ) noexcept
    {
        auto* target = runtime_->targets_.find(input.target);
        if (!target)
        {
            response.error = RenderError{kRuntimeIdentity};
            return;
        }
        auto& state = targetState(*target);
        const bool busy_writer = !state.writer.isNull() && !canChangeExecution(*runtime_->jobs_.find(state.writer));
        if (state.resizing || busy_writer)
        {
            response.error = RenderError{kDeviceBusy};
            return;
        }
        const bool invalid_epoch = input.epoch != state.epoch;
        if (input.active && (invalid_epoch || !targetAvailable(*target)))
        {
            response.error = RenderError{kRuntimeState};
            return;
        }
        state.enabled = input.active;
        if (!input.active && !state.writer.isNull())
            runtime_->jobs_.find(state.writer)->control.rejectUnadmitted(RenderError{kJobStopped});
        response = {};
    }

    void RenderServer::Impl::queryCommandIds(
        const QueryCommandIds::Request& names, QueryCommandIds::Response& response
    ) noexcept
    {
        runtime_->command_registry_.queryCommandIds(names, response);
    }

    void RenderServer::Impl::listCommands(
        const ListCommands::Request& _request,
        ListCommands::Response& output_response
    ) noexcept
    {
        output_response = runtime_->command_registry_.listCommands();
    }

    void RenderServer::Impl::progressUploads(detail::DeviceEntry& device) noexcept
    {
        if (auto* uploader = device.uploader();
            uploader && !device.upload_pending && uploader->canProgressTransfers())
        {
            device.upload_pending = true;
            const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
            stdexec::spawn(
                stdexec::starts_on(delivery, uploader->submitReadyTransfers()) |
                    stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                    stdexec::upon_stopped([]() noexcept -> RenderResult<void> { std::terminate(); }) |
                    stdexec::then(
                        [this, &device](RenderResult<void> result) noexcept
                        {
                            device.upload_pending = false;
                            if (!result)
                            {
                                failExecution(result.error());
                            }
                        }
                    ),
                upload_scope_.get_token()
            );
        }
    }

    RenderResult<void> RenderServer::Impl::applyParameterSources(std::uint32_t slot) noexcept
    {
        for (const auto& output : frame_outputs_)
        {
            const auto& content = runtime_->jobs_.find(output.job)->content;
            auto& execution = *output.execution;
            const auto& view = *runtime_->scenes_.find(content.scene)->views.find(content.view);
            auto& instance = execution.instance;
            for (const auto& imported : execution.target_images)
            {
                auto sampled = runtime_->resources_.sampledImage(runtime_->device(execution.device)->session(), {{}, imported.target}, slot);
                if (!sampled) return cxx::unexpected(sampled.error());
                instance.bindImage(imported.resource, slot, *const_cast<nri::Texture*>(sampled->image));
            }
            if (execution.camera_revision != view.camera_revision)
            {
                detail::CameraParameters camera;
                if (auto encoded = detail::projectCameraParameters(view.camera,
                    std::as_writable_bytes(std::span{&camera, 1})); !encoded) return encoded;
                if (auto applied = instance.setParameterSource(EParameterSource::VIEW,
                    std::as_bytes(std::span{&camera, 1})); !applied) return applied;
                if (instance.usesParameterSource(EParameterSource::VIEW))
                {
                    const auto& viewport = view.camera.viewport;
                    instance.setViewport({viewport[0], viewport[1], viewport[2], viewport[3]});
                }
                execution.camera_revision = view.camera_revision;
            }
            const auto& effect = execution.recipe.effect;
            const auto revision = effect.parameterRevision();
            if (execution.effect_parameter_revision != revision)
            {
                if (auto applied = instance.setParameterSource(EParameterSource::EFFECT, effect.parameterBytes());
                    !applied)
                {
                    return applied;
                }
                execution.effect_parameter_revision = revision;
            }
        }
        return {};
    }

    std::uint32_t RenderServer::Impl::outputSlot(TargetId target, std::uint32_t frame_slot) const noexcept
    {
        if (std::ranges::find(frame_outputs_, target, &FrameOutput::target) != frame_outputs_.end()) return frame_slot;
        return publishedOutputSlot(target);
    }

    std::uint32_t RenderServer::Impl::publishedOutputSlot(TargetId target) const noexcept
    {
        const auto* published = runtime_->job_outputs_.tryGet(target);
        const auto* offscreen = std::get_if<detail::OffscreenTarget>(runtime_->targets_.find(target));
        if (!published || !offscreen) return UINT32_MAX;
        const bool same_identity = published->target == target && published->target_epoch == offscreen->state.epoch;
        if (!same_identity || !offscreen->matches(published->image_index, published->content_revision))
            return UINT32_MAX;
        return published->image_index;
    }

    bool RenderServer::Impl::publishedOutputReadable(TargetId target, std::uint32_t slot) const noexcept
    {
        const auto* offscreen = std::get_if<detail::OffscreenTarget>(runtime_->targets_.find(target));
        return offscreen && offscreen->readable(slot);
    }

    bool RenderServer::Impl::resourcesAvailable(std::span<const ResourceInstanceId> resources) const noexcept
    {
        return std::ranges::none_of(resources, [&](auto id) { return runtime_->resources_.hasExclusiveCommand(id); });
    }

    bool RenderServer::Impl::frameUsesResource(ResourceInstanceId id) const noexcept
    {
        if (!frame_run_count_) return false;
        return std::ranges::any_of(frame_outputs_, [&](const auto& output)
        {
            const auto& resources = output.execution->resources;
            return std::ranges::find(resources, id) != resources.end();
        });
    }

    bool RenderServer::Impl::frameUsesTarget(TargetId id) const noexcept
    {
        if (!frame_run_count_) return false;
        return std::ranges::any_of(frame_outputs_, [&](const auto& output)
        {
            return output.target == id || std::ranges::find(output.reads, id) != output.reads.end();
        });
    }

    bool RenderServer::Impl::preparationUsesTarget(TargetId id) const noexcept
    {
        return std::ranges::find(preparation_targets_, id) != preparation_targets_.end();
    }

    RenderResult<bool> RenderServer::Impl::acquireFrameOutputs(std::uint32_t slot) noexcept
    {
        for (const auto& output : frame_outputs_)
        {
            for (const auto source : output.reads)
            {
                const auto* sampled = std::get_if<detail::OffscreenTarget>(runtime_->targets_.find(source));
                if (!sampled) return cxx::unexpected(RenderError{kRuntimeIdentity});
                if (std::ranges::find(frame_outputs_, source, &FrameOutput::target) != frame_outputs_.end()) continue;
                const auto selected = outputSlot(source, slot);
                if (selected == UINT32_MAX) return cxx::unexpected(RenderError{kRuntimeState});
                if (!sampled->readable(selected)) return false;
            }
            auto& target = *runtime_->targets_.find(output.target);
            if (auto* surface = std::get_if<detail::SwapchainTarget>(&target))
            {
                const auto ready = surface->bindImage(slot);
                if (!ready || !*ready) return ready;
            }
            else if (!std::get<detail::OffscreenTarget>(target).writable(slot))
            {
                return false;
            }
        }
        for (const auto& output : frame_outputs_)
        {
            auto& execution = *output.execution;
            for (const auto& input : execution.target_images)
            {
                const auto selected = outputSlot(input.target, slot);
                if (selected == UINT32_MAX) return cxx::unexpected(RenderError{kRuntimeState});
                const auto image = targetOutput(*runtime_->targets_.find(input.target)).frames[selected].image;
                execution.instance.bindImage(input.resource, slot, *image);
            }
        }
        return true;
    }

    void RenderServer::Impl::progressTargetFlights() noexcept
    {
        using NativeStep = detail::SwapchainTarget::ENativeStep;
        const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        for (auto& target : runtime_->targets_)
        {
            auto* surface = std::get_if<detail::SwapchainTarget>(&target);
            if (!surface || runtime_->device(surface->state.device) != runtime_->execution_device_) continue;
            const auto& writer = surface->state;
            const bool borrowed = frame_run_count_ && std::ranges::any_of(frame_outputs_, [&](const auto& output)
            {
                return runtime_->targets_.find(output.target) == &target;
            });
            const auto* job = runtime_->jobs_.find(writer.writer);
            const bool ready = writer.enabled && job && job->readyToRun();
            const auto step = surface->beginAdvance(borrowed || ready);
            if (step == NativeStep::NONE) continue;
            auto operation = stdexec::starts_on(delivery, surface->advance(step)) |
                stdexec::then([this](RenderResult<bool> result) noexcept
                {
                    if (result && *result) presentations_.fetch_add(1, std::memory_order_relaxed);
                    if (!result && detail::isNativeFailure(result.error(), nri::Result::DEVICE_LOST))
                        failExecution(result.error());
                }) |
                stdexec::upon_error([](auto&&) noexcept { std::terminate(); }) |
                stdexec::upon_stopped([]() noexcept { std::terminate(); });
            stdexec::spawn(
                stdexec::write_env(
                    std::move(operation), stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                ),
                command_scope_.get_token()
            );
        }
    }

    exec::task<RenderResult<void>> RenderServer::Impl::submitCurrentFrame() noexcept
    {
        if (!round_.operation->active())
        {
            const auto error = round_.error.type ? round_.error : RenderError{kCommandCancelled};
            co_return cxx::unexpected(error);
        }
        const auto slot = round_.slot();
        const auto& submissions = runtime_->execution_device_->frameWorkspace().submissions;
        const auto submitted_before = submissions.submittedBatches(slot);
        const auto accepted = co_await round_.operation->run(frame_instances_, frame_storages_, frame_job_order_);
        submissions_.fetch_add(submissions.submittedBatches(slot) - submitted_before, std::memory_order_relaxed);
        for (std::size_t i = 0; i < frame_outputs_.size(); ++i)
        {
            auto& output = frame_outputs_[i];
            auto* offscreen = std::get_if<detail::OffscreenTarget>(runtime_->targets_.find(output.target));
            if (output.submitted) continue;
            const auto receipt = submissions.acceptedFrameUse(slot, *frame_instances_[i], target_frame_uses_);
            output.submitted = receipt.completed;
            for (auto source : output.reads)
            {
                auto* sampled = runtime_->targets_.find(source);
                const auto selected = outputSlot(source, slot);
                if (sampled && selected != UINT32_MAX)
                {
                    if (auto* image = std::get_if<detail::OffscreenTarget>(sampled))
                        image->acceptRead(selected, target_frame_uses_);
                }
            }
            if (!offscreen) continue;
            if (receipt.wrote_output) offscreen->acceptWrite(slot, target_frame_uses_);
            if (!offscreen->description().sampled || !receipt.wrote_output) continue;
            const auto* published = runtime_->job_outputs_.tryGet(output.target);
            if (receipt.completed)
            {
                const auto revision = offscreen->publish(slot);
                const detail::JobOutput publication{output.target, slot, offscreen->state.epoch, revision};
                runtime_->job_outputs_.insert(output.target, publication);
            }
            else if (published && published->image_index == slot)
            {
                runtime_->job_outputs_.erase(output.target);
            }
        }
        if (!accepted) co_return cxx::unexpected(accepted.error());
        co_return RenderResult<void>{};
    }

    void RenderServer::Impl::queryExecutionProgress(
        const QueryExecutionProgress::Request& _request,
        QueryExecutionProgress::Response& output_response
    ) noexcept
    {
        const auto* job = runtime_->jobs_.find(_request);
        if (!job)
        {
            output_response.error = {kJobIdentity};
            return;
        }
        if (!job->execution)
        {
            output_response.error = {kJobStopped};
            return;
        }
        const auto program_error = job->execution->programError();
        output_response = {{}, job->execution->instance.progress(), program_error.type ? program_error : job->last_run_error};
    }

    void RenderServer::Impl::queryRenderStats(
        const QueryRenderStats::Request& _request,
        QueryRenderStats::Response& output_response
    ) noexcept
    {
        output_response = RenderStatsSnapshot{
            {},
            ticks_.load(std::memory_order_relaxed),
            commands_.load(std::memory_order_relaxed),
            compiled_graphs_.load(std::memory_order_relaxed),
            submissions_.load(std::memory_order_relaxed)
        };
    }

    void RenderServer::Impl::createDeviceSession(
        const CreateDeviceSession::Request& options, CreateDeviceSession::Response& response
    ) noexcept
    {
        const bool valid_backend = options.backend == EGraphicsBackend::VULKAN ||
            options.backend == EGraphicsBackend::D3D12;
        if (!valid_backend)
        {
            response.error = {kDeviceArgument};
            return;
        }

        const nri::CallbackInterface callbacks{Impl::diagnostic, [](void*) noexcept {}, this};
        // Frame copies and standalone staging use a worker independent of dynamic PSO batches.
        auto entry = detail::DeviceEntry::create(runtime_->resources_, options, callbacks, config_, recording_scheduler_);
        if (!entry)
        {
            response.error = entry.error();
            return;
        }
        if (stop_requested_.load())
        {
            response.error = {kCommandCancelled};
            return;
        }
        auto* published = entry->get();
        const auto id = runtime_->devices_.emplace(std::move(*entry));
        if (!runtime_->execution_device_) selectExecutionDevice(published);
        response = {{}, id};
    }

    void RenderServer::Impl::removeDeviceSession(
        const RemoveDeviceSession::Request& id, RemoveDeviceSession::Response& response
    ) noexcept
    {
        auto* entry = runtime_->device(id);
        if (!entry)
        {
            response.error = {kRuntimeIdentity};
            return;
        }
        const bool is_execution_device = runtime_->execution_device_ == entry;
        const bool has_targets = std::ranges::any_of(runtime_->targets_, [id](const auto& target) noexcept
        {
            return targetState(target).device == id;
        });
        const bool has_bound_jobs = std::ranges::any_of(runtime_->jobs_, [id](const auto& job) noexcept
        {
            return job.execution && job.execution->device == id;
        });
        const bool has_borrows = entry->pending_starts || entry->upload_pending || entry->resources().hasCommands() ||
            entry->scopedResourceCommands();
        const bool has_pending_work = !entry->nativeIdle() || entry->session().pendingGpuWork();
        if (has_targets || has_bound_jobs || has_borrows || has_pending_work)
        {
            response.error = {kDeviceBusy};
            return;
        }

        if (is_execution_device) selectExecutionDevice(nullptr);
        runtime_->devices_.erase(id);
        if (is_execution_device && !runtime_->devices_.empty())
        {
            selectExecutionDevice(runtime_->devices_.begin()->get());
        }
        response = {};
    }

    void RenderServer::Impl::selectExecutionDevice(detail::DeviceEntry* entry) noexcept
    {
        runtime_->execution_device_ = entry;
        ++job_structure_revision_;
        target_frame_uses_ = entry ? entry->session().makeFrontiers() : detail::SubmissionFrontiers{};
    }

    void RenderServer::Impl::queryRenderDevice(
        const QueryRenderDevice::Request& input, QueryRenderDevice::Response& response
    ) noexcept
    {
        const auto* entry = runtime_->device(input);
        if (!entry)
        {
            response.error = {kRuntimeIdentity};
            return;
        }
        const auto& session = entry->session();
        const auto& description = session.description();
        response = {
            {},
            description.graphicsAPI == nri::GraphicsAPI::D3D12 ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN,
            session.resourceCapabilities(),
            description.dimensions.attachmentMaxDim,
            static_cast<std::uint32_t>(session.queueCount()),
            entry->frameProgress().frameCount()
        };
    }

    void RenderServer::Impl::stopCommand(const Stop::Request&, Stop::Response& response) noexcept
    {
        // Stop admission and drain accepted work. Cancellation during shutdown is
        // not itself an execution fault and should not emit execution_failed.
        stop();
        response = {};
    }

    void RenderServer::Impl::cancelActiveFrame(RenderError error) noexcept
    {
        if (round_.operation) round_.operation->cancel(error);
    }

    void RenderServer::Impl::failExecution(RenderError error) noexcept
    {
        if (execution_error_.type) return;
        execution_error_ = error;
        stop();
        if (round_.operation && !round_.error.type) round_.error = error;
        cancelActiveFrame(error);
        if (config_.execution_failed)
        {
            config_.execution_failed(config_.failure_context, error);
        }
    }

    RenderResult<HandlerCall> RenderServer::Impl::resolveCommandCall(
        CommandId command,
        const CommandRecord& entry,
        const ipc::IpcMessage& request,
        ResourceCommandUse& resource
    ) noexcept
    {
        if (entry.storage)
        {
            const auto id = entry.storage(request);
            const bool exclusive = entry.description->requires_idle_resource;
            if (exclusive && frameUsesResource(id))
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }
            auto borrowed = runtime_->resources_.acquireCommand(id, command, exclusive);
            if (!borrowed) return cxx::unexpected(borrowed.error());
            const auto* selected = entry.binding(borrowed->binding().binding);
            const bool is_invalid_call = !selected || !selected->call.invoke ||
                selected->storage_type != borrowed->implementationType();
            if (is_invalid_call) return cxx::unexpected(RenderError{kCommandContract});
            resource = std::move(*borrowed);
            return selected->call;
        }
        if (!entry.effect)
        {
            const auto* binding = entry.binding(0);
            const bool is_invalid_call = !binding || !binding->callable();
            if (is_invalid_call)
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            return binding->call;
        }
        const auto* effect = runtime_->effects_.find(entry.effect(request));
        if (!effect)
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        const auto method = std::ranges::find(effect->commands, command, &CommandBindingLocation::command);
        if (method == effect->commands.end())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto* binding = entry.binding(method->binding);
        const bool is_invalid_call = !binding || !binding->callable();
        if (is_invalid_call)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        // Resource operations require their own instance identity and never use Effect selection.
        if (binding->storage_type.isValid()) return cxx::unexpected(RenderError{kCommandContract});
        return binding->call;
    }

    void RenderServer::Impl::dispatchRequest(ipc::ReceivedRequest received) noexcept
    {
        const ResponseRoute route{received.requesterId(), RequestId{received.correlation()}};
        const auto id = commandIdFromTag(received.tag());
        const auto* entry = runtime_->command_registry_.findCommand(id);
        if (!entry || route.request_id.isNull())
        {
            response_outbox_.emplaceResponse<AckResponse>(route, AckResponse{RenderError{kCommandContract}});
        }
        else if (received.message().typeHash() != entry->description->request.hash())
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kCommandContract});
        }
        else if (const auto* device = runtime_->execution_device_;
                 device && device->session().terminalError().type && !entry->description->failure_safe)
        {
            entry->make_error(response_outbox_.prepare(route), device->session().terminalError());
        }
        else
        {
            ResourceCommandUse resource;
            auto call = resolveCommandCall(id, *entry, received.message(), resource);
            if (!call)
            {
                entry->make_error(response_outbox_.prepare(route), call.error());
            }
            else
            {
                CommandDispatchContext context{
                    received.message(),
                    route,
                    response_outbox_,
                    command_scope_,
                    server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery),
                    std::move(resource)
                };
                // Async Handler construction consumes this exact call now; completion never
                // re-reads the capability directory or a selected Handler.
                call->invoke(call->object, context);
            }
        }
        commands_.fetch_add(1, std::memory_order_relaxed);
    }

    void RenderServer::Impl::shutdown() noexcept
    {
        stop_requested_.store(true);
        const auto cause = execution_error_.type ? execution_error_ : RenderError{kCommandCancelled};
        if (round_.operation && !round_.error.type) round_.error = cause;
        for (auto& job : runtime_->jobs_)
        {
            stopJob(job);
        }
        command_scope_.close();
        command_scope_.request_stop();
        cancelActiveFrame(RenderError{kUploadStopped});
        std::atomic<bool> joined{false};

        struct Joined final
        {
            using receiver_concept = stdexec::receiver_t;
            std::atomic<bool>* done;

            auto get_env() const noexcept
            {
                return stdexec::env{stdexec::prop{stdexec::get_scheduler, stdexec::inline_scheduler{}}};
            }

            void set_value() && noexcept
            {
                done->store(true, std::memory_order_release);
                ipc::IpcCore::instance().notifyWork();
            }
        };

        auto join = stdexec::connect(command_scope_.join(), Joined{&joined});
        stdexec::start(join);
        auto& notifications = ipc::IpcCore::instance();
        const auto has_pending_delivery = [&]() noexcept
        {
            const bool pending_operations = round_.operation ||
                !input_updates_.empty() || preparing_execution_;
            const bool native_pending = std::ranges::any_of(runtime_->devices_, [](const auto& device) noexcept
            {
                return device->upload_pending || !device->nativeIdle();
            });
            const bool pending_contexts = !server_context_.is_idle() || native_pending;
            return pending_operations || pending_contexts || !response_outbox_.empty();
        };
        while (!joined.load(std::memory_order_acquire) || has_pending_delivery())
        {
            const auto observed = notifications.workEpoch();
            if (!serviceOperations(false))
            {
                waitForService(observed);
            }
        }
        upload_scope_.close();
        bool uploads_joined = false;

        struct UploadJoined
        {
            using receiver_concept = stdexec::receiver_t;
            bool* done;

            auto get_env() const noexcept
            {
                return stdexec::env{stdexec::prop{stdexec::get_scheduler, stdexec::inline_scheduler{}}};
            }

            void set_value() && noexcept
            {
                *done = true;
            }
        };

        auto upload_join = stdexec::connect(upload_scope_.join(), UploadJoined{&uploads_joined});
        stdexec::start(upload_join);
        if (!uploads_joined)
        {
            std::terminate();
        }
        // All sources of new native work have returned. Staging and native payloads can now release in owner order.
        for (const auto& device : runtime_->devices_)
            if (auto drained = device->session().drain(); !drained) failExecution(drained.error());
        runtime_.reset();
        (void)server_context_.close();
        responder_.reset();
        notifications.notifyWork(); // Wake bootstrap/reply waiters after CLOSED is published.
        state_.store(EState::FINISHED);
    }

    std::size_t RenderServer::Impl::publishResponses(std::size_t limit) noexcept
    {
        const auto result = response_outbox_.flushTo(responder_, limit);
        if (result.published != 0 && config_.responses_available)
        {
            config_.responses_available(config_.response_context);
        }
        return result.settled;
    }

    bool RenderServer::Impl::serviceOperations(bool admitting) noexcept
    {
        constexpr std::size_t continuation_budget = 128;
        constexpr std::size_t response_budget = 32;
        const auto submitted_before = submissions_.load(std::memory_order_relaxed);
        // The budget limits adopted nodes. A continuation itself must return promptly.
        bool progressed = server_context_.run_ready(continuation_budget) != 0;
        for (auto& device : runtime_->devices_)
        {
            if (auto completed = device->session().progressCompletions(); !completed) failExecution(completed.error());
        }
        if (runtime_->execution_device_)
        {
            if (admitting && !stop_requested_.load()) progressTargetFlights();
            progressed |= progressInputUpdates();
            if (admitting) progressed |= progressProgramPreparations();
            progressed |= progressFrame();
        }
        for (auto& device : runtime_->devices_) progressUploads(*device);
        progressed |= server_context_.run_ready(continuation_budget) != 0;
        progressed |= publishResponses(response_budget) != 0;
        return progressed || submissions_.load(std::memory_order_relaxed) != submitted_before;
    }

    void RenderServer::Impl::waitForService(std::uint64_t observed) const noexcept
    {
        // IPC, ManualContext and stop publish the same epoch. GPU completion has no host
        // notification, and IPC scans a bounded Link prefix, so both keep a finite revisit.
        const bool has_gpu_work = std::ranges::any_of(runtime_->devices_, [](const auto& device) noexcept
        {
            return device->session().pendingGpuWork();
        });
        const auto delay = has_gpu_work
            ? std::chrono::nanoseconds(config_.gpu_wait_ns) : std::chrono::milliseconds(1);
        ipc::IpcCore::instance().waitUntil(observed, std::chrono::steady_clock::now() + delay);
    }

    RenderResult<void> RenderServer::Impl::run() noexcept
    {
        auto expected = EState::SETUP;
        if (!state_.compare_exchange_strong(expected, EState::RUNNING))
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (!stop_requested_.load())
        {
            runtime_->command_registry_.activateHandlers(runtime_->resources_);
        }
        auto& notifications = ipc::IpcCore::instance();
        while (!stop_requested_.load())
        {
            const auto observed = notifications.workEpoch();
            bool progressed = false;
            for (std::uint32_t i = 0; i < 32 && !stop_requested_.load(); ++i)
            {
                ipc::ReceivedRequest request;
                if (responder_.tryReceive(request) == ipc::EIpcReceive::RECEIVED)
                {
                    dispatchRequest(std::move(request));
                    progressed = true;
                }
            }
            if (stop_requested_.load()) break;
            progressed |= serviceOperations(true);
            ticks_.fetch_add(1, std::memory_order_relaxed);
            if (!progressed && !stop_requested_.load())
            {
                waitForService(observed);
            }
        }
        if (const auto* device = runtime_->execution_device_)
            if (const auto terminal = device->session().terminalError(); terminal.type) failExecution(terminal);
        shutdown();
        if (execution_error_.type) return cxx::unexpected(execution_error_);
        return {};
    }

    RenderServerStats RenderServer::Impl::stats() const noexcept
    {
        return {
            ticks_.load(std::memory_order_relaxed),
            commands_.load(std::memory_order_relaxed),
            compiled_graphs_.load(std::memory_order_relaxed),
            submissions_.load(std::memory_order_relaxed),
            validation_errors_.load(std::memory_order_relaxed),
            validation_warnings_.load(std::memory_order_relaxed),
            storages_created_.load(std::memory_order_relaxed),
            presentations_.load(std::memory_order_relaxed)
        };
    }

    std::optional<RenderDevelopmentStats> RenderServer::Impl::developmentStats() const noexcept
    {
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        return RenderDevelopmentStats{
            input_rebuilds_.load(std::memory_order_relaxed),
            input_rebuild_ns_.load(std::memory_order_relaxed),
            job_selection_ns_.load(std::memory_order_relaxed),
            jobs_considered_.load(std::memory_order_relaxed),
            jobs_selected_.load(std::memory_order_relaxed),
            dependency_edges_.load(std::memory_order_relaxed),
            rounds_opened_.load(std::memory_order_relaxed),
            slot_cpu_hold_ns_.load(std::memory_order_relaxed),
            0,
            program_preparation_ns_.load(std::memory_order_relaxed),
            raster_buffer_ranges_.load(std::memory_order_relaxed),
            raster_buffer_bytes_.load(std::memory_order_relaxed),
            raster_image_ranges_.load(std::memory_order_relaxed),
            raster_image_subresources_.load(std::memory_order_relaxed),
            program_preparations_.load(std::memory_order_relaxed),
            discarded_preparations_.load(std::memory_order_relaxed),
            job_selection_calls_.load(std::memory_order_acquire),
            job_selection_growths_.load(std::memory_order_relaxed),
            job_topology_growths_.load(std::memory_order_relaxed)
        };
#else
        return std::nullopt;
#endif
    }

} // namespace lux::render
