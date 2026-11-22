#include <lux/engine/render/device/RasterProgramUpdate.hpp>
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
    RenderServer::Impl::Impl(
        ServerInstanceId instance_id, RenderServerConfig value, ipc::IpcResponder input,
        execution::ExecutionRuntime& execution
    )
        : config_(std::move(value)), cpu_scheduler_(execution.cpuScheduler()),
          program_scheduler_(execution.cpuSchedulerOnWorker(0)),
          recording_scheduler_(execution.cpuSchedulerOnWorker(execution.cpuConcurrency() > 1 ? 1 : 0)),
          recording_workers_(recording_scheduler_), responder_(std::move(input)), instance_id_(instance_id),
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
        bindFrameworkCommand<QueryRenderJobInputLayout, &Impl::queryFrameInputLayout>();
        bindFrameworkCommand<UpdateFrameData, &Impl::updateFrameData>();
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
            auto& gpu = runtime_->device(execution.device)->jobResources(execution.binding_identity);
            auto changed = detail::RenderGraphInstance::ResizeResources::prepare(
                gpu, execution.instance,
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
            if (!execution) continue;
            if (execution->bindsTarget(source))
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
        const bool busy_target = state.resizing || frameUsesTarget(input.target);
        if (busy_target) co_return response(RenderError{kDeviceBusy});
        auto affected = jobsBindingTarget(input.target);
        const bool different_kind = input.description.kind != targetDescription(*target).kind;
        auto& device = *runtime_->device(state.device);
        if (const auto failure = device.admissionError(); failure.type) co_return response(failure);
        const auto maximum = device.session().description().dimensions.attachmentMaxDim;
        const bool oversized = input.description.width > maximum || input.description.height > maximum;
        const bool removes_sampling = targetDescription(*target).sampled && !input.description.sampled &&
            std::ranges::any_of(affected, [&](auto job)
            {
                return runtime_->jobs_.find(job)->execution->target != input.target;
            });
        if (different_kind || oversized || removes_sampling) co_return response(RenderError{kCommandContract});
        auto* swapchain = std::get_if<detail::SwapchainTarget>(target);
        const bool healthy = !swapchain || !swapchain->needsRecreation();
        if (input.description == targetDescription(*target) && healthy) co_return response();
        if (!state.writer.isNull())
        {
            if (!runtime_->jobs_.find(state.writer)->execution->executable.compatible(input.description))
                co_return response(RenderError{kDeviceArgument});
        }
        const bool busy_execution = std::ranges::any_of(affected, [&](auto id)
        {
            const auto& job = *runtime_->jobs_.find(id);
            return job.control.hasRun() || executionTargetsResizing(*job.execution);
        });
        if (busy_execution) co_return response(RenderError{kDeviceBusy});
        if (state.epoch == UINT64_MAX) co_return response(RenderError{kRuntimeCapacity});

        const auto writer_at_admission = state.writer;
        std::vector<detail::RenderGraphInstance*> instances;
        for (auto id : affected)
        {
            auto& execution = *runtime_->jobs_.find(id)->execution;
            instances.push_back(&execution.instance);
        }
        state.resizing = true;
        cxx::scope_exit finish([&]() noexcept
        {
            targetState(*target).resizing = false;
            for (auto id : affected)
            {
                auto& job = *runtime_->jobs_.find(id);
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
        device.outputs.erase(input.target);
        co_return response();
    }

    exec::task<AckResponse> RenderServer::Impl::removeTarget(RemoveTarget::Request&& input) noexcept
    {
        auto* target = runtime_->targets_.find(input);
        if (!target) co_return AckResponse{RenderError{kRuntimeIdentity}};
        auto& state = targetState(*target);
        const bool busy_target = state.resizing || frameUsesTarget(input);
        if (busy_target) co_return AckResponse{RenderError{kDeviceBusy}};
        if (!jobsBindingTarget(input).empty()) co_return AckResponse{RenderError{kDeviceBusy}};
        state.resizing = true;
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
        device.outputs.erase(input);
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
                                failDevice(device, result.error());
                            }
                        }
                    ),
                upload_scope_.get_token()
            );
        }
    }

    RenderResult<void> RenderServer::Impl::selectGraphInputs(detail::DeviceEntry& device, std::uint32_t slot) noexcept
    {
        auto& round = device.round;
        for (const auto read_slot : round.selected_sources)
        {
            auto& read = round.reads[read_slot.index];
            auto& publication = read.publication;
            const auto* source = std::get_if<detail::OffscreenTarget>(runtime_->targets_.find(publication.target));
            const bool available = source && source->description().sampled && !source->state.resizing;
            if (!available) return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (source->state.device != device.id) return cxx::unexpected(RenderError{kJobState});
            if (read.producer != FrameInputs::no_job)
            {
                const auto& writer = round.outputs[read.producer];
                if (writer.target != publication.target) return cxx::unexpected(RenderError{kJobState});
                publication.image_index = slot;
                publication.target_epoch = source->state.epoch;
            }
            else
            {
                const auto* published = device.outputs.tryGet(publication.target);
                const bool current = published && published->target == publication.target &&
                    published->target_epoch == source->state.epoch &&
                    source->matches(published->image_index, published->content_revision);
                if (!current) return cxx::unexpected(RenderError{kRuntimeState});
                publication = *published;
            }
            read.image = source->output().frames[publication.image_index].image;
            if (!read.image) return cxx::unexpected(RenderError{kRuntimeState});
        }
        for (std::size_t i = 0; i < round.outputs.size(); ++i)
        {
            const auto& output = round.outputs[i];
            for (const auto& import : round.inputs.jobs[output.input_job].imports)
                round.instances[i]->bindImage(import.resource, slot, *round.reads[import.source.index].image);
        }
        return {};
    }

    std::uint32_t RenderServer::Impl::publishedOutputSlot(TargetId target) const noexcept
    {
        const auto* offscreen = std::get_if<detail::OffscreenTarget>(runtime_->targets_.find(target));
        if (!offscreen) return UINT32_MAX;
        const auto* published = runtime_->device(offscreen->state.device)->outputs.tryGet(target);
        if (!published) return UINT32_MAX;
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

    bool RenderServer::Impl::frameUsesResource(const detail::DeviceEntry& device, ResourceInstanceId id) const noexcept
    {
        if (!device.round.run_count) return false;
        return std::ranges::any_of(device.round.outputs, [&](const auto& output)
        {
            const auto& resources = device.round.inputs.jobs[output.input_job].retained;
            return std::ranges::find(resources, id) != resources.end();
        });
    }

    bool RenderServer::Impl::frameUsesTarget(TargetId id) const noexcept
    {
        const auto* target = runtime_->targets_.find(id);
        if (!target) return false;
        const auto& round = runtime_->device(targetState(*target).device)->round;
        if (!round.run_count) return false;
        const bool writes = std::ranges::any_of(round.outputs, [&](const auto& output) { return output.target == id; });
        if (writes) return true;
        return std::ranges::any_of(round.selected_sources, [&](const auto slot) noexcept
        {
            return round.reads[slot.index].publication.target == id;
        });
    }

    RenderResult<bool> RenderServer::Impl::acquireFrameOutputs(detail::DeviceEntry& device, std::uint32_t slot) noexcept
    {
        const auto& round = device.round;
        for (const auto read_slot : round.selected_sources)
        {
            const auto& read = round.reads[read_slot.index];
            const auto& selected = read.publication;
            const auto* source = std::get_if<detail::OffscreenTarget>(runtime_->targets_.find(selected.target));
            if (!source || source->state.epoch != selected.target_epoch)
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            if (read.producer != FrameInputs::no_job) continue;
            if (!source->matches(selected.image_index, selected.content_revision))
                return cxx::unexpected(RenderError{kRuntimeState});
            if (!source->readable(selected.image_index)) return false;
        }
        for (const auto& output : round.outputs)
        {
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
        return true;
    }

    void RenderServer::Impl::progressTargetFlights(detail::DeviceEntry& device) noexcept
    {
        using NativeStep = detail::SwapchainTarget::ENativeStep;
        const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        for (auto& target : runtime_->targets_)
        {
            auto* surface = std::get_if<detail::SwapchainTarget>(&target);
            if (!surface || surface->state.device != device.id) continue;
            const auto& writer = surface->state;
            const bool borrowed = device.round.run_count && std::ranges::any_of(device.round.outputs, [&](const auto& output)
            {
                return runtime_->targets_.find(output.target) == &target;
            });
            const auto* job = runtime_->jobs_.find(writer.writer);
            const bool ready = writer.enabled && job && job->readyToRun();
            const auto step = surface->beginAdvance(borrowed || ready);
            if (step == NativeStep::NONE) continue;
            auto operation = stdexec::starts_on(delivery, surface->advance(step)) |
                stdexec::then([this, id = device.id](RenderResult<bool> result) noexcept
                {
                    if (result && *result) presentations_.fetch_add(1, std::memory_order_relaxed);
                    if (!result && detail::isNativeFailure(result.error(), nri::Result::DEVICE_LOST))
                        if (auto* owner = runtime_->device(id)) failDevice(*owner, result.error());
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

    exec::task<RenderResult<void>> RenderServer::Impl::submitCurrentFrame(detail::DeviceEntry& device) noexcept
    {
        auto& round = device.round;
        if (!round.operation->active())
        {
            const auto error = round.error.type ? round.error : RenderError{kCommandCancelled};
            co_return cxx::unexpected(error);
        }
        const auto slot = round.slot();
        const auto& submissions = device.frameWorkspace().submissions;
        const auto submitted_before = submissions.submittedBatches(slot);
        const auto uniform_state = [this](EffectId id, graph::NodeId node, cxx::basic_type_info type) noexcept
            -> const void*
        {
            const auto* effect = runtime_->effects_.find(id);
            if (!effect) return nullptr;
            const auto found = std::ranges::find(effect->node_states, node, &RenderEffect::NodeState::node);
            return found != effect->node_states.end() && found->type == type ? found->value.get() : nullptr;
        };
        const auto accepted = co_await round.operation->run(
            round.instances, round.storages, round.job_order, detail::FrameOperation::UniformStateLookup{uniform_state}
        );
        submissions_.fetch_add(submissions.submittedBatches(slot) - submitted_before, std::memory_order_relaxed);
        RenderError settlement_error{};
        for (std::size_t i = 0; i < round.outputs.size(); ++i)
        {
            auto& output = round.outputs[i];
            auto* offscreen = std::get_if<detail::OffscreenTarget>(runtime_->targets_.find(output.target));
            if (output.submitted) continue;
            const auto receipt = submissions.acceptedFrameUse(slot, *round.instances[i], round.target_uses);
            output.submitted = receipt.completed;
            const auto& execution = *runtime_->jobs_.find(output.job)->execution;
            const auto active_write = [&](const detail::FinalBufferUse& use) noexcept
            {
                return use.write && submissions.acceptedPass(slot, execution.instance, receipt, use.pass);
            };
            auto& registry = runtime_->resources_;
            for (const auto& link : execution.productUses())
            {
                registry.acceptProductUse(link, slot, round.target_uses);
                for (const auto& use : link.uses)
                    if (active_write(use)) registry.invalidateProduct(link, slot, use.bytes);
            }
            if (receipt.completed)
            {
                for (const auto& link : execution.productUses())
                    for (const auto& exported : link.exports)
                    {
                        if (exported.when && !execution.instance.frozenCondition(slot, *exported.when)) continue;
                        const bool produced = std::ranges::any_of(link.uses, [&](const auto& use)
                        {
                            const bool overlaps = use.bytes.offset < exported.bytes.offset + exported.bytes.size &&
                                exported.bytes.offset < use.bytes.offset + use.bytes.size;
                            return overlaps && active_write(use);
                        });
                        if (produced) registry.publishProduct(link, {
                            output.job, round.frame.frame_index, slot, exported.bytes
                        });
                    }
            }
            for (const auto source_slot : round.inputs.readsFor(round.inputs.jobs[output.input_job]))
            {
                const auto& selected = round.reads[source_slot.index].publication;
                auto* source = std::get_if<detail::OffscreenTarget>(runtime_->targets_.find(selected.target));
                if (!source || source->state.epoch != selected.target_epoch)
                {
                    settlement_error = {kRuntimeIdentity};
                    continue;
                }
                source->acceptRead(selected.image_index, round.target_uses);
            }
            if (!offscreen) continue;
            if (receipt.wrote_output) offscreen->acceptWrite(slot, round.target_uses);
            if (!offscreen->description().sampled || !receipt.wrote_output) continue;
            const auto* published = device.outputs.tryGet(output.target);
            if (receipt.completed)
            {
                const auto revision = offscreen->publish(slot);
                const detail::JobOutput publication{output.target, slot, offscreen->state.epoch, revision};
                device.outputs.insert(output.target, publication);
            }
            else if (published && published->image_index == slot)
            {
                device.outputs.erase(output.target);
            }
        }
        if (!accepted) co_return cxx::unexpected(accepted.error());
        if (settlement_error.type) co_return cxx::unexpected(settlement_error);
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
        const auto& instance = job->execution->instance;
        const bool report_program = program_error.type && !instance.hasExclusiveProgramInput();
        output_response = {{}, instance.progress(), report_program ? program_error : job->last_run_error};
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
        published->id = id;
        response = {{}, id};
    }

    exec::task<AckResponse> RenderServer::Impl::removeDeviceSession(RemoveDeviceSession::Request&& id) noexcept
    {
        auto* entry = runtime_->device(id);
        if (!entry) co_return AckResponse{RenderError{kRuntimeIdentity}};
        if (entry->retiring) co_return AckResponse{RenderError{kDeviceBusy}};
        const bool has_targets = std::ranges::any_of(runtime_->targets_, [id](const auto& target) noexcept
        {
            return targetState(target).device == id;
        });
        const bool preparing = std::ranges::any_of(runtime_->effects_, [id](const auto& effect) noexcept
        {
            return effect.preparing_device == id;
        });
        if (has_targets || preparing || entry->hasBorrows())
            co_return AckResponse{RenderError{kDeviceBusy}};

        entry->retiring = true;
        for (auto& effect : runtime_->effects_)
            std::erase_if(effect.device_graphs, [id](const auto& installed) { return installed.device == id; });
        auto& session = entry->session();
        auto accepted = session.makeFrontiers();
        for (std::size_t index = 0; index < session.queueCount(); ++index)
        {
            accepted.accept(session.submissionPoint(index, session.physicalQueue(index).submitted()));
        }
        // Deliver after completion callbacks have returned and keep the accepted removal alive during Stop.
        const auto owner = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        const auto retired = co_await (
            session.waitFrontiers(std::move(accepted)) |
            stdexec::continues_on(owner) |
            stdexec::write_env(stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}})
        );
        if (!retired)
        {
            const auto error = retired.error();
            const bool safe = error.type == kOutcomeUnknown || detail::isNativeFailure(error, nri::Result::DEVICE_LOST);
            if (!safe) std::terminate();
        }
        if (entry->hasBorrows()) std::terminate();

        runtime_->devices_.erase(id);
        co_return AckResponse{};
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
        if (!round.operation) return;
        if (!round.error.type) round.error = error;
        round.operation->cancel(error);
    }

    void RenderServer::Impl::failDevice(detail::DeviceEntry& device, RenderError error) noexcept
    {
        if (device.failure.type) return;
        device.failure = error;
        cancelActiveFrame(device, error);
        if (device.program_update) device.program_update->stop.request_stop();
        if (device.retiring) return;

        for (auto id : running_jobs_)
        {
            auto& job = *runtime_->jobs_.find(id);
            if (job.execution->device != device.id) continue;
            job.last_run_error = error;
            job.control.rejectUnadmitted(error);
        }
        const bool has_healthy_device = std::ranges::any_of(runtime_->devices_, [](const auto& entry) noexcept
        {
            return !entry->admissionError().type;
        });
        if (!has_healthy_device) failExecution(error);
    }

    void RenderServer::Impl::failExecution(RenderError error) noexcept
    {
        if (execution_error_.type) return;
        execution_error_ = error;
        stop();
        if (config_.execution_failed)
        {
            config_.execution_failed(config_.failure_context, error);
        }
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
            const auto* session = runtime_->resources_.deviceOf(id);
            if (!session) return cxx::unexpected(RenderError{kStorageIdentity});
            const auto found = std::ranges::find_if(runtime_->devices_, [session](const auto& owner) noexcept
            {
                return &owner->session() == session;
            });
            if (found == runtime_->devices_.end()) return cxx::unexpected(RenderError{kStorageIdentity});
            const auto& device = **found;
            if (device.retiring) return cxx::unexpected(RenderError{kDeviceBusy});
            const auto failure = device.admissionError();
            if (failure.type && !entry.description->failure_safe) return cxx::unexpected(failure);
            const bool exclusive = entry.description->requires_idle_resource;
            if (exclusive && frameUsesResource(device, id))
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }
            auto& resources = runtime_->resources_;
            const auto admitted = resources.beginCommand(id, command, exclusive);
            if (!admitted) return cxx::unexpected(admitted.error());
            const auto* selected = entry.binding(admitted->binding.binding);
            const bool is_invalid_call = !selected || !selected->call.invoke ||
                selected->storage_type != admitted->implementation;
            if (is_invalid_call)
            {
                resources.finishCommand(id);
                return cxx::unexpected(RenderError{kCommandContract});
            }
            context.resource_id = id;
            context.resource = resources.resourceInstance(id);
            context.device = found->get();
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
        if (!node.valid()) return cxx::unexpected(RenderError{kCommandContract});
        const auto* call = effect->findCommand(command, node);
        if (!call) return cxx::unexpected(RenderError{kCommandContract});
        return *call;
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
                received.message(), route, response_outbox_, command_scope_,
                server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery), runtime_->resources_, {}
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
        commands_.fetch_add(1, std::memory_order_relaxed);
    }

    void RenderServer::Impl::shutdown() noexcept
    {
        stop_requested_.store(true);
        const auto cause = execution_error_.type ? execution_error_ : RenderError{kCommandCancelled};

        for (auto& job : runtime_->jobs_)
        {
            stopJob(job);
        }
        command_scope_.close();
        command_scope_.request_stop();
        for (auto& device : runtime_->devices_) cancelActiveFrame(*device, cause);
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
            const bool device_pending = std::ranges::any_of(runtime_->devices_, [](const auto& device) noexcept
            {
                return device->round.operation ||
                    device->program_update || device->upload_pending || !device->nativeIdle();
            });
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
            if (!surface) continue;
            auto retire = stdexec::starts_on(delivery, surface->close()) |
                stdexec::then([this, id = surface->state.device](RenderResult<void> result) noexcept
                {
                    if (!result) failDevice(*runtime_->device(id), result.error());
                }) |
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
            if (!serviceOperations(false)) waitForService(observed);
        }
        for (const auto& job : runtime_->jobs_)
        {
            if (job.execution) std::terminate();
        }
        for (auto& effect : runtime_->effects_)
        {
            if (!effect.preparing_device.isNull()) std::terminate();
            effect.device_graphs.clear();
        }
        // All sources of new native work have returned. Staging and native payloads can now release in owner order.
        for (const auto& device : runtime_->devices_)
        {
            if (auto drained = device->session().drain(); !drained) failDevice(*device, drained.error());
        }
        for (auto& device : runtime_->devices_) device->clearUniformPools();
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
        auto& devices = runtime_->devices_;
        if (!devices.empty())
        {
            const auto service = [&](detail::DeviceEntry& device) noexcept
            {
                const auto completed = device.session().progressCompletions();
                const auto failure = completed ? device.session().terminalError() : completed.error();
                if (failure.type) failDevice(device, failure);
                const bool can_admit = admitting && !stop_requested_.load() && !device.admissionError().type;
                if (can_admit) progressTargetFlights(device);

                progressed |= settleProgramCompilation(device);
                if (can_admit) progressed |= progressProgramPreparations(device);
                progressed |= progressFrame(device);
                if (!device.retiring) progressUploads(device);
            };
            const auto start = next_device_ % devices.size();
            const auto turn = std::next(devices.begin(), start);
            for (auto it = turn; it != devices.end(); ++it) service(**it);
            for (auto it = devices.begin(); it != turn; ++it) service(**it);
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
        effect_catalog_.seal();
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
        for (auto& device : runtime_->devices_)
            if (const auto terminal = device->session().terminalError(); terminal.type) failDevice(*device, terminal);
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
            job_topology_growths_.load(std::memory_order_relaxed),
            raster_geometry_ranges_.load(std::memory_order_relaxed),
            raster_geometry_bytes_.load(std::memory_order_relaxed)
        };
#else
        return std::nullopt;
#endif
    }

} // namespace lux::render
