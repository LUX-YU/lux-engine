#include <lux/engine/render/device/SubmissionQueue.hpp>
#include <exception>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/NativeResult.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/graph/compile/RenderGraphCompiler.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <ranges>

namespace lux::render
{
    RenderServer::Impl::Impl(
        ServerInstanceId instance_id,
        RenderServerConfig value,
        ipc::IpcResponder input,
        execution::ExecutionRuntime& execution
    )
        : config_(std::move(value)), cpu_scheduler_(execution.cpuScheduler()),
          recording_workers_(
              execution.cpuSchedulerOnWorker(0), execution.cpuSchedulerOnWorker(execution.cpuConcurrency() > 1 ? 1 : 0)
          ), responder_(std::move(input)), instance_id_(instance_id),
          server_context_(
              cxx::execution::default_manual_context_config(),
              {nullptr, [](void*) noexcept { ipc::IpcCore::instance().notifyWork(); }}
          )
    {
        const auto query = bindFrameworkCommand<QueryCommandIds, &Impl::queryCommandIds>();
        if (query != QueryCommandIds::id)
        {
            std::terminate(); // Fixed bootstrap identity is a protocol invariant.
        }
        bindFrameworkCommand<QueryResource, &Impl::queryResource>();
        bindFrameworkCommand<ListResourceJobs, &Impl::listResourceJobs>();
        bindFrameworkCommand<RemoveResource, &Impl::removeResource>();
        bindFrameworkCommand<GetOrCreateResource, &Impl::getOrCreateResource>();
        bindFrameworkCommand<CreateScene, &Impl::createScene>();
        bindFrameworkCommand<RemoveScene, &Impl::removeScene>();
        bindFrameworkCommand<CreateView, &Impl::createView>();
        bindFrameworkCommand<UpdateView, &Impl::updateView>();
        frame_update_command_ = bindFrameworkCommand<UpdateFrameData, &Impl::updateFrameData>();
        bindFrameworkCommand<RemoveView, &Impl::removeView>();
        bindFrameworkCommand<CreateTarget, &Impl::createTarget>();
        bindFrameworkCommand<ResizeTarget, &Impl::resizeTarget>();
        bindFrameworkCommand<RemoveTarget, &Impl::removeTarget>();
        bindFrameworkCommand<SetTargetActive, &Impl::setTargetActive>();
        bindEffectGraphCommands();
        bindFrameworkCommand<RemoveEffect, &Impl::removeEffect>();
        bindJobCommands();
        bindFrameworkCommand<Stop, &Impl::stopCommand>();
        bindFrameworkCommand<QueryRenderStats, &Impl::queryRenderStats>();
        bindFrameworkCommand<CreateDeviceSession, &Impl::createDeviceSession>();
        bindFrameworkCommand<RemoveDeviceSession, &Impl::removeDeviceSession>();
        bindFrameworkCommand<QueryRenderDevice, &Impl::queryRenderDevice>();
        bindFrameworkCommand<QueryExecutionProgress, &Impl::queryExecutionProgress>();
        bindFrameworkCommand<ListEffectCapabilities, &Impl::listEffectCapabilities>();
        bindFrameworkCommand<ListCommands, &Impl::listCommands>();
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

    void RenderServer::Impl::createTarget(const CreateTarget::Request& input, CreateTarget::Response& response) noexcept
    {
        auto* entry = runtime_->device(input.device);
        if (!entry)
        {
            response.error = {kRuntimeIdentity};
            return;
        }
        if (const auto failure = entry->admissionError(); failure.type)
        {
            response.error = failure;
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
        float sdr_white_nits = 0.F;
        if (is_surface)
        {
            auto physical = detail::SwapchainTarget::create(
                entry->session(),
                input.description,
                *input.surface,
                entry->frameProgress().frameCount()
            );
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            sdr_white_nits = physical->sdrWhiteNits();
            id = runtime_->targets_.emplace(std::move(*physical));
        }
        else
        {
            auto physical = detail::OffscreenTarget::create(
                entry->session(),
                input.description,
                entry->frameProgress().frameCount()
            );
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            id = runtime_->targets_.emplace(std::move(*physical));
        }
        targetState(*runtime_->targets_.find(id)).device = input.device;
        response = TargetCreated{{}, id, 1, targetDescription(*runtime_->targets_.find(id)), true, sdr_white_nits};
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
            std::vector<detail::ImageReplacement> inputs;
            for (const auto& image : execution.target_images)
            {
                if (image.target == target)
                {
                    inputs.push_back({image.operand.resource, output.frames});
                }
            }
            auto changed = detail::ResizeResources::prepare(
                execution,
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

    std::vector<RenderJobId> RenderServer::Impl::jobsBindingTarget(TargetId source) const noexcept
    {
        std::vector<RenderJobId> result;
        for (const auto& job : runtime_->jobs_)
        {
            const auto* execution = job.execution.get();
            if (!execution)
            {
                continue;
            }
            if (execution->bindsTarget(source))
            {
                result.push_back(job.id());
            }
        }
        return result;
    }

    exec::task<TargetCreated> RenderServer::Impl::resizeTarget(ResizeTarget::Request&& input) noexcept
    {
        auto* target = runtime_->targets_.find(input.target);
        if (!target)
        {
            co_return TargetCreated{RenderError{kRuntimeIdentity}};
        }
        auto* swapchain = std::get_if<detail::SwapchainTarget>(target);
        const auto response = [&](RenderError error = {}) noexcept
        {
            // The target variant may have adopted a new physical row since admission.
            const auto* surface = std::get_if<detail::SwapchainTarget>(target);
            return TargetCreated{
                error,
                input.target,
                targetState(*target).epoch,
                targetDescription(*target),
                targetAvailable(*target),
                surface ? surface->sdrWhiteNits() : 0.F
            };
        };

        if (auto valid = input.description.validate(); !valid)
        {
            co_return response(valid.error());
        }

        auto& state = targetState(*target);
        const bool busy_target = state.pending_replacement.has_value() || frameUsesTarget(input.target);
        if (busy_target)
        {
            co_return response(RenderError{kDeviceBusy});
        }
        auto affected = jobsBindingTarget(input.target);
        const bool different_kind = input.description.kind != targetDescription(*target).kind;
        auto& device = *runtime_->device(state.device);
        if (const auto failure = device.admissionError(); failure.type)
        {
            co_return response(failure);
        }
        const auto maximum = device.session().description().dimensions.attachmentMaxDim;
        const bool oversized = input.description.width > maximum || input.description.height > maximum;
        if (different_kind || oversized)
        {
            co_return response(RenderError{kCommandContract});
        }
        const bool mode_change = input.description.display_mode != targetDescription(*target).display_mode;
        const bool rebind_writer = !input.output_effect.isNull();
        if (input.display_changed && !swapchain) co_return response(RenderError{kCommandContract});
        if (rebind_writer && (!swapchain || state.writer.isNull()))
            co_return response(RenderError{kCommandContract});
        if (swapchain)
        {
            const auto display = swapchain->preflightPresentation(input.description.display_mode, input.display_changed);
            if (!display)
            {
                if (!mode_change)
                {
                    state.enabled = false;
                    if (!state.writer.isNull()) rejectPendingRun(*runtime_->jobs_.find(state.writer), display.error());
                }
                co_return response(display.error());
            }
        }
        const bool healthy = !swapchain || !swapchain->needsRecreation();
        if (input.description == targetDescription(*target) && healthy && !input.display_changed)
        {
            if (rebind_writer)
            {
                auto& writer = *runtime_->jobs_.find(state.writer);
                co_return response(rebindJob({device.id, writer.id(), input.output_effect, input.target}).error);
            }
            co_return response();
        }
        const auto& current = targetDescription(*target);
        const bool same_contract = current.display_mode == input.description.display_mode &&
            current.outputContract() == input.description.outputContract();
        if (!same_contract)
        {
            // Installed readers retain the sampled format and sample count even after the writer stops.
            const bool has_reader = std::ranges::any_of(affected, [&](auto id) noexcept
            {
                return runtime_->jobs_.find(id)->execution->readsTarget(input.target);
            });
            if (has_reader)
                co_return response(RenderError{input.description.sampled ? kDeviceArgument : kCommandContract});
        }
        if (!state.writer.isNull())
        {
            const auto& writer = *runtime_->jobs_.find(state.writer);
            if (rebind_writer)
            {
                if (!canChangeExecution(writer) || hasDeferredRun(writer.id())) co_return response(RenderError{kJobBusy});
                const auto* effect = runtime_->effects_.find(input.output_effect);
                if (!effect) co_return response(RenderError{kJobIdentity});
                const auto* prepared = effect->graphForDevice(device.id);
                if (!prepared) co_return response(RenderError{kJobState});
                // DXGI's pinned SDR and linear HDR recipes have fixed formats. Reject incompatible graphs before close.
                auto expected = input.description;
                if (mode_change)
                    expected.format = expected.display_mode == EDisplayMode::HDR ? rdesc::ETextureFormat::RGBA16_SFLOAT :
                        rdesc::ETextureFormat::RGBA8_UNORM;
                const bool incompatible_output = effect->output != expected.outputContract() ||
                    prepared->graph->output() == kGraphInvalidIndex || !prepared->graph->compatible(expected.graphRequirements());
                if (incompatible_output) co_return response(RenderError{kDeviceArgument});
            }
            else if (!same_contract || !writer.execution->graph().compatible(input.description.graphRequirements()))
                co_return response(RenderError{kDeviceArgument});
        }
        const bool busy_execution = std::ranges::any_of(
            affected,
            [&](auto id)
            {
                const auto& job = *runtime_->jobs_.find(id);
                return job.hasRun() || executionTargetsResizing(*job.execution);
            }
        );
        if (busy_execution)
        {
            co_return response(RenderError{kDeviceBusy});
        }
        if (state.epoch == UINT64_MAX)
        {
            co_return response(RenderError{kRuntimeCapacity});
        }

        const auto writer_at_admission = state.writer;
        const bool was_enabled = state.enabled;
        auto accepted = device.session().makeFrontiers();
        for (auto id : affected)
        {
            auto& execution = *runtime_->jobs_.find(id)->execution;
            accepted.merge(device.jobResources(execution.binding_identity).accepted);
        }
        state.pending_replacement = input.output_effect;
        cxx::scope_exit finish(
            [&]() noexcept
            {
                targetState(*target).pending_replacement.reset();
                for (auto id : affected)
                {
                    auto& job = *runtime_->jobs_.find(id);
                    retireStoppedExecution(job);
                }
            }
        );

        std::optional<VRenderTarget> candidate_target;
        ResizeCandidates candidates;
        if (auto* offscreen = std::get_if<detail::OffscreenTarget>(target))
        {
            offscreen->appendUses(accepted);
            auto created = detail::OffscreenTarget::create(
                device.session(),
                input.description,
                device.frameProgress().frameCount()
            );
            if (!created)
            {
                co_return response(created.error());
            }
            candidate_target.emplace(std::move(*created));
            auto prepared = prepareTargetResize(input.target, targetOutput(*candidate_target), affected);
            if (!prepared)
            {
                co_return response(prepared.error());
            }
            candidates = std::move(*prepared);
        }
        if (auto waited = co_await device.session().waitFrontiers(accepted); !waited)
        {
            co_return response(waited.error());
        }
        if (stop_requested_.load())
        {
            co_return response(RenderError{kCommandCancelled});
        }
        if (swapchain)
        {
            if (rebind_writer && state.writer != writer_at_admission) co_return response(RenderError{kJobStopped});
            const auto window = swapchain->surface();
            if (!writer_at_admission.isNull())
            {
                auto& writer = *runtime_->jobs_.find(writer_at_admission);
                writer.execution->releaseOutputReferences();
                rejectPendingRun(writer, {kJobStopped});
            }
            state.enabled = false;
            auto closed = co_await swapchain->close();
            if (!closed)
            {
                co_return response(closed.error());
            }
            if (stop_requested_.load())
            {
                co_return response(RenderError{kCommandCancelled});
            }
            auto created = detail::SwapchainTarget::create(
                device.session(),
                input.description,
                window,
                device.frameProgress().frameCount()
            );
            if (!created)
            {
                co_return response(created.error());
            }
            candidate_target.emplace(std::move(*created));
            if (rebind_writer)
            {
                if (state.writer != writer_at_admission) co_return response(RenderError{kJobStopped});
                auto& writer = *runtime_->jobs_.find(writer_at_admission);
                auto& next = targetState(*candidate_target);
                next = state;
                ++next.epoch;
                next.enabled = was_enabled;
                const auto installed = buildJobExecution({
                    device.id, writer.id(), input.output_effect, input.target, writer.binding, EJobStartMode::REBIND
                }, &*candidate_target);
                co_return response(installed.error);
            }
            auto prepared = prepareTargetResize(input.target, targetOutput(*candidate_target), affected);
            if (!prepared)
            {
                co_return response(prepared.error());
            }
            candidates = std::move(*prepared);
        }
        for (auto& [id, candidate] : candidates)
        {
            if (auto ready = candidate.prepareAdoption(*runtime_->jobs_.find(id)->execution); !ready)
            {
                co_return response(ready.error());
            }
        }
        for (auto& [id, candidate] : candidates)
        {
            std::move(candidate).commit(*runtime_->jobs_.find(id)->execution);
        }

        auto next = state;
        ++next.epoch;
        next.enabled = false;

        if (!next.writer.isNull())
        {
            rejectPendingRun(*runtime_->jobs_.find(next.writer), {kJobStopped});
        }

        targetState(*candidate_target) = next;
        adoptTarget(*target, std::move(*candidate_target));
        co_return response();
    }

    exec::task<AckResponse> RenderServer::Impl::removeTarget(RemoveTarget::Request&& input) noexcept
    {
        auto* target = runtime_->targets_.find(input);
        if (!target)
        {
            co_return AckResponse{RenderError{kRuntimeIdentity}};
        }
        auto& state = targetState(*target);
        const bool busy_target = state.pending_replacement.has_value() || frameUsesTarget(input);
        if (busy_target)
        {
            co_return AckResponse{RenderError{kDeviceBusy}};
        }
        if (!jobsBindingTarget(input).empty())
        {
            co_return AckResponse{RenderError{kDeviceBusy}};
        }
        state.pending_replacement.emplace();
        auto& device = *runtime_->device(state.device);
        RenderError retired;
        if (auto* surface = std::get_if<detail::SwapchainTarget>(target))
        {
            if (auto closed = co_await surface->close(); !closed)
            {
                retired = closed.error();
                failDevice(device, retired);
            }
        }
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
        if (state.pending_replacement.has_value() || busy_writer)
        {
            response.error = RenderError{kDeviceBusy};
            return;
        }
        if (input.active)
        {
            if (const auto failure = runtime_->device(state.device)->admissionError(); failure.type)
            {
                response.error = failure;
                return;
            }
        }
        const bool invalid_epoch = input.epoch != state.epoch;
        if (input.active && (invalid_epoch || !targetAvailable(*target)))
        {
            response.error = RenderError{kRuntimeState};
            return;
        }
        if (input.active && targetDescription(*target).display_mode == EDisplayMode::HDR)
        {
            auto& surface = std::get<detail::SwapchainTarget>(*target);
            if (auto display = surface.preflightPresentation(EDisplayMode::HDR); !display)
            {
                state.enabled = false;
                response.error = display.error();
                return;
            }
        }
        state.enabled = input.active;
        if (!input.active && !state.writer.isNull())
        {
            rejectPendingRun(*runtime_->jobs_.find(state.writer), {kJobStopped});
        }
        response = {};
    }

    void RenderServer::Impl::queryCommandIds(
        const QueryCommandIds::Request& names,
        QueryCommandIds::Response& response
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
        if (auto task = device.startReadyUploads())
        {
            const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
            stdexec::spawn(
                stdexec::starts_on(delivery,
                    std::move(*task)) |
                    stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                    stdexec::upon_stopped([]() noexcept -> RenderResult<void> { std::terminate(); }) |
                    stdexec::then(
                        [this, &device](RenderResult<void> result) noexcept
                        {
                            device.upload_pending = false;
                            if (!result)
                            {
                                failDevice(device, result.error());
                            }
                        }
                    ),
                upload_scope_.get_token()
            );
        }
    }

    void RenderServer::Impl::bindSelectedTargets(detail::DeviceEntry& device, std::uint32_t slot) noexcept
    {
        auto& round = device.round;
        for (std::size_t i = 0; i < round.outputs.size(); ++i)
        {
            const auto& output = round.outputs[i];
            if (auto* offscreen = std::get_if<detail::OffscreenTarget>(&output.job.target))
                offscreen->bindOutput(slot, output.image_index);
            else std::get<detail::SwapchainTarget>(output.job.target).bindReadyImage(slot);
            auto& execution = *round.executions[i];
            for (const auto& import : std::span{output.job.imports}.first(output.job.static_imports))
            {
                const auto& source = round.inputs.sources[import.source.index];
                execution.frame_bindings.bind(import.operand->resource, slot, source.selected.input);
            }
            for (auto& read : execution.frame(slot).target_reads)
            {
                const auto& import = output.job.imports[read.import_index];
                const auto& source = round.inputs.sources[import.source.index];
                read.requested = {source.target, source.read};
                read.wait_stages = read.batch == import.batch ? import.wait_stages : nri::StageBits::NONE;
                read.view = source.selected.view;
                read.uses = source.selected.input.queue_uses;
            }
        }
    }

    bool RenderServer::Impl::resourcesAvailable(const FrameInputs::Job& job) const noexcept
    {
        const auto busy = [&](auto id) noexcept { return runtime_->resources_.hasExclusiveCommand(id); };
        return std::ranges::none_of(job.direct_resources, busy) && std::ranges::none_of(job.indirect_resources, busy);
    }

    bool RenderServer::Impl::frameUsesResource(
        const detail::DeviceEntry& device, ResourceInstanceId id, bool include_frozen
    ) const noexcept
    {
        const auto& round = device.round;
        for (std::size_t i = 0; i < round.executions.size(); ++i)
        {
            if (!include_frozen && round.executions[i]->frame(round.slot()).frozen_for_submission) continue;
            if (round.outputs[i].job.uses(id)) return true;
        }
        return false;
    }

    bool RenderServer::Impl::awaitingInputRead(ResourceInstanceId id) const noexcept
    {
        const auto* device = resourceDevice(id);
        return device && frameUsesResource(*device, id, false);
    }

    bool RenderServer::Impl::awaitingInputRead(const FrameData& packet) const noexcept
    {
        for (const auto& batch : packet.batches())
        {
            if (batch.target.server != instance_id_ || !awaitingInputRead(batch.target.instance)) continue;
            for (std::size_t i = 0; i < batch.channels.size(); ++i)
                if (packet.column(batch.first_column + i).count) return true;
        }
        return false;
    }

    ResourceInstanceId
    RenderServer::Impl::mutableInput(const CommandRecord& entry, const ipc::IpcMessage& message) const noexcept
    {
        const auto& desc = *entry.description;
        const bool is_update = entry.storage && !desc.requires_idle_resource && !desc.failure_safe &&
            message.typeHash() == desc.request.hash();
        if (!is_update) return {};
        const auto id = entry.storage(message);
        const auto* binding = entry.resourceBinding(runtime_->resources_.declarationOf(id));
        return binding && binding->call.synchronous ? id : ResourceInstanceId{};
    }

    bool RenderServer::Impl::awaitingInputRead(CommandId id, const ipc::IpcMessage& message) const noexcept
    {
        if (id == frame_update_command_)
        {
            if (message.typeHash() != cxx::make_basic_type_info<UpdateFrameData::Request>().hash()) return false;
            return awaitingInputRead(message.get<UpdateFrameData::Request>().data);
        }
        const auto* entry = runtime_->command_registry_.findCommand(id);
        if (!entry) return false;
        const auto resource = mutableInput(*entry, message);
        return !resource.isNull() && awaitingInputRead(resource);
    }

    bool RenderServer::Impl::frameUsesTarget(TargetId id) const noexcept
    {
        const auto* target = runtime_->targets_.find(id);
        if (!target)
        {
            return false;
        }
        const auto& round = runtime_->device(targetState(*target).device)->round;
        if (round.executions.empty())
        {
            return false;
        }
        const auto& inputs = round.inputs;
        const auto* writer = inputs.writers.tryGet(id);
        if (writer && inputs.selection[*writer].selected != FrameInputs::no_job) return true;
        return std::ranges::any_of(inputs.source_slots, [&](const auto& slots) noexcept
        {
            const auto* index = slots.tryGet(id);
            return index && inputs.sources[*index].selected.input.image;
        });
    }

    void RenderServer::Impl::progressTargetFlights(detail::DeviceEntry& device) noexcept
    {
        using NativeStep = detail::SwapchainTarget::ENativeStep;
        const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        for (auto& target : runtime_->targets_)
        {
            auto* surface = std::get_if<detail::SwapchainTarget>(&target);
            if (!surface || surface->state.device != device.id)
            {
                continue;
            }
            const auto& writer = surface->state;
            const bool borrowed =
                !device.round.executions.empty() &&
                std::ranges::any_of(
                    device.round.outputs,
                    [&](const auto& output) { return &output.job.target == &target; }
                );
            const auto* job = runtime_->jobs_.find(writer.writer);
            const bool ready = writer.enabled && job && job->readyToRun();
            const auto step = surface->beginAdvance(borrowed || ready);
            if (step == NativeStep::NONE)
            {
                continue;
            }
            auto operation = stdexec::starts_on(delivery, surface->advance(step)) |
                             stdexec::then(
                                 [this, id = device.id](RenderResult<bool> result) noexcept
                                 {
                                     if (result && *result)
                                     {
                                         presentations_.fetch_add(1, std::memory_order_relaxed);
                                     }
                                     if (!result && detail::isNativeFailure(result.error(), nri::Result::DEVICE_LOST))
                                     {
                                         if (auto* owner = runtime_->device(id))
                                         {
                                             failDevice(*owner, result.error());
                                         }
                                     }
                                 }
                             ) |
                             stdexec::upon_error([](auto&&) noexcept { std::terminate(); }) |
                             stdexec::upon_stopped([]() noexcept { std::terminate(); });
            stdexec::spawn(
                stdexec::write_env(
                    std::move(operation),
                    stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                ),
                command_scope_.get_token()
            );
        }
    }

    exec::task<RenderResult<void>> RenderServer::Impl::submitCurrentFrame(detail::DeviceEntry& device) noexcept
    {
        auto& round = device.round;
        if (!round.active())
        {
            const auto error = round.error.type ? round.error : RenderError{kCommandCancelled};
            co_return cxx::unexpected(error);
        }
        const auto slot = round.slot();
        const auto& submissions = round.submissions;
        const auto submitted_before = submissions.submittedBatches(slot);
        const auto accepted = co_await round.submitNativeWork(cpu_scheduler_, &recording_workers_);
        const auto submitted = submissions.submittedBatches(slot) - submitted_before;
        if (submitted) submissions_.fetch_add(submitted, std::memory_order_relaxed);
        if (!submitted && accepted) co_return RenderResult<void>{};
        for (std::size_t i = 0; i < round.outputs.size(); ++i)
        {
            auto& output = round.outputs[i];
            auto* offscreen = std::get_if<detail::OffscreenTarget>(&output.job.target);
            if (output.submitted)
            {
                continue;
            }
            const auto& execution = *round.executions[i];
            const auto* receipt = submissions.acceptedFrameUse(slot, i, execution.binding_identity);
            if (!receipt) continue;
            output.submitted = receipt->completed;
            const auto enabled = [&](std::uint32_t region) noexcept
            {
                return region == kGraphInvalidIndex || execution.frame(slot).enabled_regions[region];
            };
            auto& registry = runtime_->resources_;
            for (const auto& link : execution.buffer_products)
            {
                for (const auto& write : link.slot->writes)
                {
                    if (!enabled(write.condition_region)) continue;
                    if (receipt->completed || submissions.mayHaveExecuted(slot, *receipt, write.batch))
                        registry.invalidateProduct(link, slot, write.bytes);
                }
            }
            // A complete Job has confirmed acceptance for every batch covering its exports.
            if (receipt->completed)
            {
                for (const auto& link : execution.buffer_products)
                {
                    for (const auto& exported : link.slot->exports)
                    {
                        if (exported.when && !execution.frozenCondition(slot, *exported.when))
                        {
                            continue;
                        }
                        const bool covered = exported.conditional_writes.empty() ||
                            detail::bufferWritesCover(exported.bytes, exported.conditional_writes, enabled);
                        if (covered)
                        {
                            registry.publishProduct(link, {output.job.id, round.frame.frame_index, slot, exported.bytes});
                        }
                    }
                }
                if (offscreen && receipt->wrote_output) offscreen->publish(output.image_index);
                runtime_->jobs_.find(output.job.id)->acceptSubmission();
            }
        }
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
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
        output_response = {{}, job->execution->progress, job->last_run_error};
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
        const CreateDeviceSession::Request& options,
        CreateDeviceSession::Response& response
    ) noexcept
    {
        const bool valid_backend =
            options.backend == EGraphicsBackend::VULKAN || options.backend == EGraphicsBackend::D3D12;
        if (!valid_backend)
        {
            response.error = {kDeviceArgument};
            return;
        }

        const nri::CallbackInterface callbacks{Impl::diagnostic, [](void*) noexcept {}, this};
        // Frame copies and standalone staging use a worker independent of dynamic PSO batches.
        auto entry =
            detail::DeviceEntry::create(runtime_->resources_, options, {config_, callbacks, recording_workers_, runtime_->jobs_});
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
        published->id = id;
        response = {{}, id};
    }

    exec::task<AckResponse> RenderServer::Impl::removeDeviceSession(RemoveDeviceSession::Request&& id) noexcept
    {
        auto* entry = runtime_->device(id);
        if (!entry)
        {
            co_return AckResponse{RenderError{kRuntimeIdentity}};
        }
        if (entry->retiring)
        {
            co_return AckResponse{RenderError{kDeviceBusy}};
        }
        const bool has_targets = std::ranges::any_of(
            runtime_->targets_,
            [id](const auto& target) noexcept { return targetState(target).device == id; }
        );
        const bool preparing = std::ranges::any_of(
            runtime_->effects_,
            [id](const auto& effect) noexcept { return effect.preparing_device == id; }
        );
        if (has_targets || preparing || entry->hasBorrows())
        {
            co_return AckResponse{RenderError{kDeviceBusy}};
        }

        entry->retiring = true;
        for (auto& effect : runtime_->effects_)
        {
            effect.device_graphs.erase(id);
        }
        auto& session = entry->session();
        auto accepted = session.makeFrontiers();
        for (std::size_t index = 0; index < session.queueCount(); ++index)
        {
            accepted.accept(session.submissionPoint(index, session.physicalQueue(index).submitted()));
        }
        // Deliver after completion callbacks have returned and keep the accepted removal alive during Stop.
        const auto owner = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        const auto retired = co_await (
            session.waitFrontiers(std::move(accepted)) | stdexec::continues_on(owner) |
            stdexec::write_env(stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}})
        );
        if (!retired)
        {
            const auto error = retired.error();
            const bool safe = error.type == kOutcomeUnknown || detail::isNativeFailure(error, nri::Result::DEVICE_LOST);
            if (!safe)
            {
                std::terminate();
            }
        }
        if (entry->hasBorrows())
        {
            std::terminate();
        }

        runtime_->devices_.erase(id);
        co_return AckResponse{};
    }

    void RenderServer::Impl::queryRenderDevice(
        const QueryRenderDevice::Request& input,
        QueryRenderDevice::Response& response
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
            session.resourceFeatures(),
            description.dimensions.attachmentMaxDim,
            static_cast<std::uint32_t>(session.queueCount()),
            entry->frameProgress().frameCount(),
            entry->failure.type ? entry->failure : session.terminalError()
        };
    }

    void RenderServer::Impl::stopCommand(const Stop::Request&, Stop::Response& response) noexcept
    {
        // Stop admission and drain accepted work. Cancellation during shutdown is
        // not itself an execution fault and should not emit execution_failed.
        stop();
        response = {};
    }

    void RenderServer::Impl::cancelActiveFrame(detail::DeviceEntry& device, RenderError error) noexcept
    {
        auto& round = device.round;
        if (round.stage == DeviceRound::EStage::IDLE)
        {
            return;
        }
        round.cancel(error);
    }

    void RenderServer::Impl::failDevice(detail::DeviceEntry& device, RenderError error) noexcept
    {
        if (device.failure.type)
        {
            return;
        }
        device.failure = error;
        cancelActiveFrame(device, error);
        if (device.retiring)
        {
            return;
        }

        for (auto id : running_jobs_)
        {
            auto& job = *runtime_->jobs_.find(id);
            if (job.execution->device != device.id)
            {
                continue;
            }
            job.last_run_error = error;
            job.rejectUnadmitted(error);
        }
        const bool has_healthy_device = std::ranges::any_of(
            runtime_->devices_,
            [](const auto& entry) noexcept { return !entry->admissionError().type; }
        );
        if (!has_healthy_device)
        {
            failExecution(error);
        }
    }

    void RenderServer::Impl::failExecution(RenderError error) noexcept
    {
        if (execution_error_.type)
        {
            return;
        }
        execution_error_ = error;
        stop();
        if (config_.execution_failed)
        {
            config_.execution_failed(config_.failure_context, error);
        }
    }

    detail::DeviceEntry* RenderServer::Impl::resourceDevice(ResourceInstanceId id) const noexcept
    {
        return runtime_->resources_.deviceOwner(id);
    }

    RenderResult<HandlerCall> RenderServer::Impl::resolveCommandCall(
        CommandId command,
        const CommandRecord& entry,
        CommandDispatchContext& context
    ) noexcept
    {
        if (entry.storage)
        {
            const auto id = entry.storage(context.request);
            auto* owner = resourceDevice(id);
            if (!owner) return cxx::unexpected(RenderError{kStorageIdentity});
            const auto& device = *owner;
            if (device.retiring)
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }
            const auto failure = device.admissionError();
            if (failure.type && !entry.description->failure_safe)
            {
                return cxx::unexpected(failure);
            }
            const bool exclusive = entry.description->requires_idle_resource;
            if (exclusive && frameUsesResource(device, id, true))
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }
            auto& resources = runtime_->resources_;
            const auto type = resources.declarationOf(id);
            const auto* selected = entry.resourceBinding(type);
            if (!selected) return cxx::unexpected(RenderError{kCommandContract});
            const auto admitted = resources.beginCommand(id, exclusive);
            if (!admitted) return cxx::unexpected(admitted.error());
            context.resource_id = id;
            context.resource = *admitted;
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
        const auto* effect = runtime_->effects_.find(entry.effect(context.request));
        if (!effect)
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        const auto node = entry.effect_node(context.request);
        if (!node.valid())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto* call = effect->findCommand(command, node);
        if (!call)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        return *call;
    }

    void RenderServer::Impl::rejectInput(ipc::ReceivedRequest& message, RenderError error) noexcept
    {
        const auto id = commandIdFromTag(message.tag());
        const ResponseRoute route{message.requesterId(), RequestId{message.correlation()}};
        if (id == run_command_)
        {
            rejectWindow(message.message().get<RequestRenderJob::Request>(), route, error);
            return;
        }
        auto& output = response_outbox_.prepare(route);
        if (id == frame_update_command_)
        {
            auto& packet = message.message().get<UpdateFrameData::Request>().data;
            output.emplace<UpdateFrameData::Response>(UpdateFrameData::Response{error, std::move(packet)});
        }
        else runtime_->command_registry_.findCommand(id)->make_error(output, error);
    }

    void RenderServer::Impl::receiveRequest(ipc::ReceivedRequest message) noexcept
    {
        commands_.fetch_add(1, std::memory_order_relaxed);
        const auto id = commandIdFromTag(message.tag());
        const auto* entry = runtime_->command_registry_.findCommand(id);
        const bool valid = entry && message.correlation() &&
            message.message().typeHash() == entry->description->request.hash();
        if (!valid)
        {
            dispatchRequest(std::move(message));
            return;
        }
        if (id == run_command_)
        {
            bool busy = false;
            const auto& input = message.message().get<RequestRenderJob::Request>();
            input.forEachRun({message.correlation()}, [&](const auto& run) noexcept
            {
                const auto* job = runtime_->jobs_.find(run.job);
                busy |= hasDeferredRun(run.job) || (job && job->hasRequest());
            });
            if (busy)
            {
                rejectInput(message, {kJobBusy});
                return;
            }
        }
        const auto resource = mutableInput(*entry, message.message());
        const auto name = entry->description->name.name();
        const bool frame_input = id == run_command_ || id == frame_update_command_ ||
            name == UpdateView::name || name == UpdateJobInputs::name || entry->effect || !resource.isNull();
        if (!frame_input)
        {
            dispatchRequest(std::move(message));
            return;
        }

        const bool earlier_queued = std::ranges::any_of(deferred_inputs_, [&](const auto& item) noexcept
        {
            return item.requesterId() == message.requesterId();
        });
        // Adjacent demand-only Runs may share the same product round. A queued mutation separates them.
        const bool demand_only = id == run_command_ && message.message().get<RequestRenderJob::Request>().demandOnly();
        const bool deferred = earlier_queued || (!demand_only && waitsForWindow(message.requesterId()));
        const bool reader_busy = !deferred && (id == frame_update_command_ ?
            awaitingInputRead(message.message().get<UpdateFrameData::Request>().data) :
            !resource.isNull() && awaitingInputRead(resource));
        if (deferred || reader_busy)
        {
            if (inputQueueFull()) rejectInput(message, {kRuntimeCapacity});
            else deferred_inputs_.push_back(std::move(message));
            return;
        }
        dispatchRequest(std::move(message));
    }

    bool RenderServer::Impl::inputQueueFull() const noexcept
    {
        return deferred_inputs_.size() >= config_.max_deferred_inputs - input_windows_.size();
    }

    bool RenderServer::Impl::waitsForWindow(ipc::RequesterId requester) const noexcept
    {
        return std::ranges::find(input_windows_, requester, &InputWindow::requester) != input_windows_.end();
    }

    void RenderServer::Impl::releaseInputWindows() noexcept
    {
        // Drop individual routes before dispatching new input. A client may reuse a terminal correlation
        // while the remaining members of its former window are still draining.
        for (auto& window : input_windows_)
        {
            const auto released = [&](const RequestRenderJob::Run& run) noexcept
            {
                const auto* job = runtime_->jobs_.find(run.job);
                if (!job || !job->awaits({window.requester, run.reply})) return true;
                if (!job->hasAdmittedRequest()) return false;
                const auto& round = runtime_->device(job->execution->device)->round;
                return round.active() && job->execution->frame(round.slot()).frozen_for_submission;
            };
            if (!window.lead.reply.isNull() && released(window.lead)) window.lead.reply = {};
            std::erase_if(window.additional_runs, released);
        }
        std::erase_if(input_windows_, [](const InputWindow& window) noexcept
        {
            return window.lead.reply.isNull() && window.additional_runs.empty();
        });
    }

    bool RenderServer::Impl::drainInputs() noexcept
    {
        releaseInputWindows();
        bool progressed = false;
        for (auto it = deferred_inputs_.begin(); it != deferred_inputs_.end();)
        {
            const auto requester = it->requesterId();
            const bool waiting = waitsForWindow(requester);
            const auto id = commandIdFromTag(it->tag());
            const bool demand_only = id == run_command_ && it->message().get<RequestRenderJob::Request>().demandOnly();
            const bool has_predecessor = std::any_of(deferred_inputs_.begin(), it,
                [&](const auto& item) noexcept { return item.requesterId() == requester; });
            if (has_predecessor || (waiting && !demand_only) || awaitingInputRead(id, it->message()))
            {
                ++it;
                continue;
            }
            auto message = std::move(*it);
            it = deferred_inputs_.erase(it);
            dispatchRequest(std::move(message));
            progressed = true;
        }
        return progressed;
    }

    bool RenderServer::Impl::hasDeferredRun(RenderJobId job) const noexcept
    {
        return std::ranges::any_of(deferred_inputs_, [&](const auto& item) noexcept
        {
            return commandIdFromTag(item.tag()) == run_command_ &&
                item.message().template get<RequestRenderJob::Request>().contains(job);
        });
    }

    void RenderServer::Impl::cancelDeferredRuns(RenderJobId job) noexcept
    {
        std::erase_if(deferred_inputs_, [&](auto& item) noexcept
        {
            if (commandIdFromTag(item.tag()) != run_command_) return false;
            if (!item.message().template get<RequestRenderJob::Request>().contains(job)) return false;
            rejectInput(item, {kJobStopped});
            return true;
        });
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
        else
        {
            CommandDispatchContext context{
                received.message(),
                route,
                response_outbox_,
                command_scope_,
                server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery),
                runtime_->resources_,
                {}
            };
            auto call = resolveCommandCall(id, *entry, context);
            if (!call)
            {
                entry->make_error(response_outbox_.prepare(route), call.error());
            }
            else
            {
                // Async Handler construction consumes this exact call now; completion never
                // re-reads the capability directory or a selected Handler.
                call->invoke(call->object, context);
            }
        }
    }

    void RenderServer::Impl::shutdown() noexcept
    {
        stop_requested_.store(true);
        const auto cause = execution_error_.type ? execution_error_ : RenderError{kCommandCancelled};

        for (auto& message : deferred_inputs_) rejectInput(message, cause);
        deferred_inputs_.clear();
        // The Jobs retain the updates and reply routes. Closing admission releases all pending
        // requests with the shutdown cause, while admitted Runs settle through their device.
        input_windows_.clear();
        for (auto& job : runtime_->jobs_)
        {
            stopJob(job, cause);
        }
        command_scope_.close();
        command_scope_.request_stop();
        for (auto& device : runtime_->devices_)
        {
            cancelActiveFrame(*device, cause);
        }
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
            const bool device_pending = std::ranges::any_of(
                runtime_->devices_,
                [](const auto& device) noexcept
                {
                    return device->round.stage != DeviceRound::EStage::IDLE || device->upload_pending ||
                           !device->nativeIdle();
                }
            );
            return device_pending || !server_context_.is_idle() || !response_outbox_.empty();
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

        // Retire live swapchains on their own native workers while this owner keeps polling completion.
        stdexec::simple_counting_scope presentations;
        const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        for (auto& target : runtime_->targets_)
        {
            auto* surface = std::get_if<detail::SwapchainTarget>(&target);
            if (!surface)
            {
                continue;
            }
            auto retire = stdexec::starts_on(delivery, surface->close()) |
                          stdexec::then(
                              [this, id = surface->state.device](RenderResult<void> result) noexcept
                              {
                                  if (!result)
                                  {
                                      failDevice(*runtime_->device(id), result.error());
                                  }
                              }
                          ) |
                          stdexec::upon_error([](auto&&) noexcept { std::terminate(); }) |
                          stdexec::upon_stopped([]() noexcept { std::terminate(); });
            stdexec::spawn(std::move(retire), presentations.get_token());
        }
        presentations.close();
        std::atomic<bool> presentations_joined{false};
        auto presentation_join = stdexec::connect(presentations.join(), Joined{&presentations_joined});
        stdexec::start(presentation_join);
        while (!presentations_joined.load(std::memory_order_acquire))
        {
            const auto observed = notifications.workEpoch();
            if (!serviceOperations(false))
            {
                waitForService(observed);
            }
        }
        for (const auto& job : runtime_->jobs_)
        {
            if (job.execution || !job.resources.empty())
            {
                std::terminate();
            }
        }
        runtime_->jobs_.clear();
        for (auto& effect : runtime_->effects_)
        {
            if (!effect.preparing_device.isNull())
            {
                std::terminate();
            }
            effect.device_graphs.clear();
        }
        // All sources of new native work have returned. Staging and native payloads can now release in owner order.
        for (const auto& device : runtime_->devices_)
        {
            if (auto drained = device->session().drain(); !drained)
            {
                failDevice(*device, drained.error());
            }
        }
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
        // A continuation may have released a frame's last mutable reader. Service queued writes before
        // a continuous Job can select the same resource for its next frame.
        if (progressed && admitting && !deferred_inputs_.empty() && !stop_requested_.load()) drainInputs();
        auto& devices = runtime_->devices_;
        if (!devices.empty())
        {
            const auto service = [&](detail::DeviceEntry& device) noexcept
            {
                const auto completed = device.session().progressCompletions();
                const auto failure = completed ? device.session().terminalError() : completed.error();
                if (failure.type)
                {
                    failDevice(device, failure);
                }
                const bool can_admit = admitting && !stop_requested_.load() && !device.admissionError().type;
                if (can_admit)
                {
                    progressTargetFlights(device);
                }

                progressed |= progressFrame(device);
                if (!device.retiring)
                {
                    progressUploads(device);
                }
            };
            const auto start = next_device_ % devices.size();
            const auto turn = std::next(devices.begin(), start);
            for (auto it = turn; it != devices.end(); ++it)
            {
                service(**it);
            }
            for (auto it = devices.begin(); it != turn; ++it)
            {
                service(**it);
            }
            next_device_ = start + 1;
        }
        progressed |= server_context_.run_ready(continuation_budget) != 0;
        progressed |= publishResponses(response_budget) != 0;
        return progressed || submissions_.load(std::memory_order_relaxed) != submitted_before;
    }

    void RenderServer::Impl::waitForService(std::uint64_t observed) const noexcept
    {
        // IPC, ManualContext and stop publish the same epoch. GPU completion has no host
        // notification, and IPC scans a bounded Link prefix, so both keep a finite revisit.
        const bool has_gpu_work = std::ranges::any_of(
            runtime_->devices_,
            [](const auto& device) noexcept { return device->session().pendingGpuWork(); }
        );

        const auto delay = has_gpu_work ? std::chrono::nanoseconds(config_.gpu_wait_ns) : std::chrono::milliseconds(1);

        ipc::IpcCore::instance().waitUntil(observed, std::chrono::steady_clock::now() + delay);
    }

    RenderResult<void> RenderServer::Impl::run() noexcept
    {
        auto expected = EState::SETUP;
        if (!state_.compare_exchange_strong(expected, EState::RUNNING))
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        effect_catalog_.seal();
        if (!stop_requested_.load())
        {
            runtime_->command_registry_.activateHandlers(runtime_->resources_);
        }
        auto& notifications = ipc::IpcCore::instance();
        while (!stop_requested_.load())
        {
            const auto observed = notifications.workEpoch();
            bool progressed = drainInputs();
            for (std::uint32_t i = 0; i < 32 && !stop_requested_.load(); ++i)
            {
                ipc::ReceivedRequest request;
                if (responder_.tryReceive(request) == ipc::EIpcReceive::RECEIVED)
                {
                    receiveRequest(std::move(request));
                    progressed = true;
                }
            }
            if (stop_requested_.load())
            {
                break;
            }
            progressed |= serviceOperations(true);
            ticks_.fetch_add(1, std::memory_order_relaxed);
            if (!progressed && !stop_requested_.load())
            {
                waitForService(observed);
            }
        }
        for (auto& device : runtime_->devices_)
        {
            if (const auto terminal = device->session().terminalError(); terminal.type)
            {
                failDevice(*device, terminal);
            }
        }
        shutdown();
        if (execution_error_.type)
        {
            return cxx::unexpected(execution_error_);
        }
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
            raster_buffer_ranges_.load(std::memory_order_relaxed),
            raster_buffer_bytes_.load(std::memory_order_relaxed),
            raster_image_ranges_.load(std::memory_order_relaxed),
            raster_image_subresources_.load(std::memory_order_relaxed),
            job_selection_calls_.load(std::memory_order_acquire),
            job_selection_growths_.load(std::memory_order_relaxed),
            job_topology_growths_.load(std::memory_order_relaxed),
            raster_geometry_ranges_.load(std::memory_order_relaxed),
            raster_geometry_bytes_.load(std::memory_order_relaxed)
        };
#else
        return std::nullopt;
#endif
    }

} // namespace lux::render
