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
#include <map>
#include <ranges>
#include <set>

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
        bindFrameworkCommand<QueryRenderDevice, &Impl::queryRenderDevice>();
        bindFrameworkCommand<QueryExecutionProgress, &Impl::queryExecutionProgress>();
        bindFrameworkCommand<ListCapabilities, &Impl::listCapabilities>();
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

    RenderResult<void> RenderServer::Impl::initialize(DeviceOptions device_options) noexcept
    {
        const nri::CallbackInterface callbacks{Impl::diagnostic, [](void*) noexcept {}, this};
        auto entry = detail::DeviceEntry::create(domain_->resources_, device_options, callbacks, config_, cpu_scheduler_);
        if (!entry)
        {
            return cxx::unexpected(entry.error());
        }
        domain_->device_ = std::move(*entry);
        target_frame_uses_ = domain_->device_->session().makeFrontiers();
        domain_->resources_.setOutputResolver(this, [](void* owner, TargetId id, std::uint32_t slot) noexcept
            -> RenderResult<ResourceRegistry::SampledImage>
        {
            auto& self = *static_cast<Impl*>(owner);
            const auto* target = self.domain_->targets_.find(id);
            if (!target || !targetDescription(*target).sampled || targetState(*target).resizing)
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            const auto selected = self.outputSlot(id, slot);
            if (selected == UINT32_MAX) return cxx::unexpected(RenderError{kRuntimeState});
            const auto frame = targetOutput(*target).frames[selected];
            return ResourceRegistry::SampledImage{frame.image, frame.sampled_index};
        });
        return {};
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
            const auto* target = domain_->targets_.find(id);
            const auto writer = target ? targetState(*target).writer : RenderJobId{};
            const auto* job = writer != replaced ? domain_->jobs_.find(writer) : nullptr;
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
            auto physical =
                detail::SwapchainTarget::create(domain_->device_->session(), input.description, *input.surface, domain_->device_->frameProgress().frameCount());
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            id = domain_->targets_.emplace(std::move(*physical));
        }
        else
        {
            auto physical = detail::OffscreenTarget::create(domain_->device_->session(), input.description, domain_->device_->frameProgress().frameCount());
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            id = domain_->targets_.emplace(std::move(*physical));
        }
        response = TargetCreated{{}, id, 1, targetDescription(*domain_->targets_.find(id)), true};
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
            auto& execution = *domain_->jobs_.find(id)->execution;
            std::vector<detail::RenderGraphInstance::ImageReplacement> inputs;
            for (const auto& image : execution.target_images)
            {
                if (image.target == target)
                {
                    inputs.push_back({image.resource, output.frames});
                }
            }
            auto changed = execution.instance.prepareResize(
                execution.target == target ? output : targetOutput(*domain_->targets_.find(execution.target)),
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
        for (const auto& job : domain_->jobs_)
        {
            const auto* execution = job.execution.get();
            if (!execution) continue;
            const bool binds_source = std::ranges::find(
                execution->target_images, source, &JobExecution::TargetImage::target
            ) != execution->target_images.end();
            const auto sampled = execution->instance.sampledTargets();
            if (binds_source || std::ranges::find(sampled, source) != sampled.end())
                result.push_back(job.control.id());
        }
        return result;
    }

    exec::task<TargetCreated> RenderServer::Impl::resizeTarget(ResizeTarget::Request&& input) noexcept
    {
        auto* target = domain_->targets_.find(input.target);
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
        const auto maximum = domain_->device_->session().description().dimensions.attachmentMaxDim;
        const bool oversized = input.description.width > maximum || input.description.height > maximum;
        const bool removes_sampling = !input.description.sampled && !affected.empty();
        if (different_kind || oversized || removes_sampling) co_return response(RenderError{kCommandContract});
        auto* swapchain = std::get_if<detail::SwapchainTarget>(target);
        const bool healthy = !swapchain || !swapchain->needsRecreation();
        if (input.description == targetDescription(*target) && healthy) co_return response();
        if (!state.writer.isNull())
        {
            affected.push_back(state.writer);
            if (!domain_->jobs_.find(state.writer)->execution->plan.compatible(input.description))
                co_return response(RenderError{kDeviceArgument});
        }
        const bool busy_execution = std::ranges::any_of(affected, [&](auto id)
        {
            return !canChangeExecution(*domain_->jobs_.find(id));
        });
        if (busy_execution) co_return response(RenderError{kDeviceBusy});
        if (state.epoch == UINT64_MAX) co_return response(RenderError{kRuntimeCapacity});

        const auto writer_at_admission = state.writer;
        std::vector<detail::RenderGraphInstance*> instances;
        for (auto id : affected)
        {
            auto& execution = *domain_->jobs_.find(id)->execution;
            ++execution.pending_uses;
            instances.push_back(&execution.instance);
        }
        state.resizing = true;
        active_frame_inputs_dirty_ = true;
        cxx::scope_exit finish([&]() noexcept
        {
            targetState(*target).resizing = false;
            for (auto id : affected)
            {
                auto& job = *domain_->jobs_.find(id);
                --job.execution->pending_uses;
                retireStoppedExecution(job);
            }
            active_frame_inputs_dirty_ = true;
        });

        auto accepted = domain_->device_->frameWorkspace().submissions.acceptedUses(
            domain_->device_->session(), domain_->device_->frameProgress(), instances
        );
        std::optional<VRenderTarget> candidate_target;
        ResizeCandidates candidates;
        if (auto* offscreen = std::get_if<detail::OffscreenTarget>(target))
        {
            offscreen->appendUses(accepted);
            auto created = detail::OffscreenTarget::create(domain_->device_->session(), input.description, domain_->device_->frameProgress().frameCount());
            if (!created) co_return response(created.error());
            candidate_target.emplace(std::move(*created));
            auto prepared = prepareTargetResize(input.target, targetOutput(*candidate_target), affected);
            if (!prepared) co_return response(prepared.error());
            candidates = std::move(*prepared);
        }
        if (auto waited = co_await domain_->device_->session().waitFrontiers(accepted); !waited) co_return response(waited.error());
        if (stop_requested_.load()) co_return response(RenderError{kCommandCancelled});
        if (swapchain)
        {
            const auto window = swapchain->surface();
            if (!writer_at_admission.isNull())
            {
                auto& writer = *domain_->jobs_.find(writer_at_admission);
                writer.execution->instance.releaseOutputReferences();
                writer.control.rejectUnadmitted(RenderError{kJobStopped});
            }
            state.enabled = false;
            auto closed = co_await swapchain->close();
            if (!closed) co_return response(closed.error());
            if (stop_requested_.load()) co_return response(RenderError{kCommandCancelled});
            auto created = detail::SwapchainTarget::create(domain_->device_->session(), input.description, window, domain_->device_->frameProgress().frameCount());
            if (!created) co_return response(created.error());
            candidate_target.emplace(std::move(*created));
            auto prepared = prepareTargetResize(input.target, targetOutput(*candidate_target), affected);
            if (!prepared) co_return response(prepared.error());
            candidates = std::move(*prepared);
        }
        for (auto& [id, candidate] : candidates)
        {
            if (auto ready = domain_->jobs_.find(id)->execution->instance.prepareResizeAdoption(candidate); !ready)
                co_return response(ready.error());
        }
        for (auto& [id, candidate] : candidates)
            domain_->jobs_.find(id)->execution->instance.commitResize(std::move(candidate));
        auto next = state;
        ++next.epoch;
        next.enabled = false;
        if (!next.writer.isNull())
            domain_->jobs_.find(next.writer)->control.rejectUnadmitted(RenderError{kJobStopped});
        std::visit([&](auto& value) noexcept
        {
            using T = std::remove_cvref_t<decltype(value)>;
            value.state = next;
            target->emplace<T>(std::move(value));
        }, *candidate_target);
        domain_->job_outputs_.erase(input.target);
        co_return response();
    }

    exec::task<AckResponse> RenderServer::Impl::removeTarget(RemoveTarget::Request&& input) noexcept
    {
        auto* target = domain_->targets_.find(input);
        if (!target) co_return AckResponse{RenderError{kRuntimeIdentity}};
        auto& state = targetState(*target);
        const bool busy_target = state.resizing || state.reservation ||
            frameUsesTarget(input) || preparationUsesTarget(input);
        if (busy_target) co_return AckResponse{RenderError{kDeviceBusy}};
        if (!jobsSamplingTarget(input).empty()) co_return AckResponse{RenderError{kRuntimeState}};
        if (!state.writer.isNull())
        {
            if (!canChangeExecution(*domain_->jobs_.find(state.writer))) co_return AckResponse{RenderError{kDeviceBusy}};
            stopJob(*domain_->jobs_.find(state.writer));
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
        domain_->job_outputs_.erase(input);
        domain_->targets_.erase(input);
        co_return AckResponse{retired};
    }

    void RenderServer::Impl::setTargetActive(
        const SetTargetActive::Request& input,
        SetTargetActive::Response& response
    ) noexcept
    {
        auto* target = domain_->targets_.find(input.target);
        if (!target)
        {
            response.error = RenderError{kRuntimeIdentity};
            return;
        }
        auto& state = targetState(*target);
        const bool busy_writer = !state.writer.isNull() && !canChangeExecution(*domain_->jobs_.find(state.writer));
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
            domain_->jobs_.find(state.writer)->control.rejectUnadmitted(RenderError{kJobStopped});
        active_frame_inputs_dirty_ = true;
        response = {};
    }

    void RenderServer::Impl::queryCommandIds(
        const QueryCommandIds::Request& names, QueryCommandIds::Response& response
    ) noexcept
    {
        domain_->command_registry_.queryCommandIds(names, response);
    }

    void RenderServer::Impl::listCommands(
        const ListCommands::Request& _request,
        ListCommands::Response& output_response
    ) noexcept
    {
        output_response = domain_->command_registry_.listCommands();
    }

    void RenderServer::Impl::progressUploads() noexcept
    {
        if (auto* uploader = domain_->device_->uploader();
            uploader && !upload_progress_pending_ && uploader->canProgressTransfers())
        {
            upload_progress_pending_ = true;
            const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
            stdexec::spawn(
                stdexec::starts_on(delivery, uploader->submitReadyTransfers()) |
                    stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                    stdexec::upon_stopped([]() noexcept -> RenderResult<void> { std::terminate(); }) |
                    stdexec::then(
                        [this](RenderResult<void> result) noexcept
                        {
                            upload_progress_pending_ = false;
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
            const auto& content = domain_->jobs_.find(output.job)->content;
            auto& execution = *output.execution;
            const auto& view = *domain_->scenes_.find(content.scene)->views.find(content.view);
            auto& instance = execution.instance;
            for (const auto& imported : execution.target_images)
            {
                auto sampled = domain_->resources_.sampledImage({{}, imported.target}, slot);
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

    void RenderServer::Impl::rebuildActiveFrameInputs() noexcept
    {
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto rebuild_started_at = std::chrono::steady_clock::now();
#endif
        frame_input_error_ = {};
        output_uses_revision_ = domain_->resources_.outputUsesRevision();
        // Semantic conflict order is stable. Fairness rotates ready physical queues, not
        // the order of writers of one imported resource.
        std::vector<RenderJobId> active;
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> job_positions;
        for (auto id : running_jobs_)
        {
            const auto& job = *domain_->jobs_.find(id);
            if (job.readyToRun() && jobTargetReady(job))
            {
                job_positions.emplace(std::pair{id.index, id.gen}, active.size());
                active.push_back(id);
            }
        }
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto selection_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - rebuild_started_at
        ).count();
        job_selection_ns_.fetch_add(selection_ns, std::memory_order_relaxed);
        jobs_considered_.fetch_add(running_jobs_.size(), std::memory_order_relaxed);
#endif
        std::map<std::uint32_t, std::size_t> positions;
        struct ReadinessEdge final
        {
            std::size_t consumer;
            bool accepts_history = false;
        };
        std::vector<std::vector<ReadinessEdge>> successors(active.size());
        std::vector<std::size_t> predecessors(active.size());
        std::vector<bool> unavailable(active.size());
        std::vector<RenderError> input_errors(active.size());
        for (std::size_t i = 0; i < active.size(); ++i)
        {
            positions.emplace(domain_->jobs_.find(active[i])->execution->target.index, i);
        }
        for (auto id : unready_jobs_)
        {
            const auto position = job_positions.find({id.index, id.gen});
            if (position != job_positions.end()) unavailable[position->second] = true;
        }
        for (std::size_t i = 0; i < active.size(); ++i)
        {
            const auto& job = *domain_->jobs_.find(active[i]);
            const auto& execution = *job.execution;
            for (auto predecessor : job.relations.predecessors)
            {
                const auto found = job_positions.find({predecessor.index, predecessor.gen});
                if (found != job_positions.end())
                {
                    successors[found->second].push_back({i});
                    ++predecessors[i];
                }
            }
            auto sources = execution.instance.sampledTargets();
            for (const auto& input : execution.target_images)
                if (std::ranges::find(sources, input.target) == sources.end()) sources.push_back(input.target);
            for (auto source : sources)
            {
                const auto* target = domain_->targets_.find(source);
                const auto producer = positions.find(source.index);
                if (!target)
                {
                    unavailable[i] = true;
                    input_errors[i] = {kRuntimeIdentity};
                    continue;
                }
                if (targetState(*target).resizing)
                {
                    unavailable[i] = true;
                    continue;
                }
                const bool has_history = publishedOutputSlot(source) != UINT32_MAX;
                if (producer == positions.end())
                {
                    if (!has_history)
                    {
                        unavailable[i] = true;
                        input_errors[i] = {kRuntimeState};
                    }
                    continue;
                }
                successors[producer->second].push_back({i, has_history});
                ++predecessors[i];
            }
        }
        std::vector<std::size_t> ready;
        std::vector<RenderJobId> ordered;
        ready.reserve(active.size());
        ordered.reserve(active.size());
        for (std::size_t i = 0; i < active.size(); ++i)
        {
            if (predecessors[i] == 0)
            {
                ready.push_back(i);
            }
        }
        for (std::size_t cursor = 0; cursor < ready.size(); ++cursor)
        {
            const auto i = ready[cursor];
            if (!unavailable[i])
            {
                ordered.push_back(active[i]);
            }
            else if (input_errors[i].type)
            {
                auto& job = *domain_->jobs_.find(active[i]);
                job.control.rejectUnadmitted(input_errors[i]);
            }
            for (const auto [consumer, accepts_history] : successors[i])
            {
                if (unavailable[i] && !accepts_history)
                {
                    unavailable[consumer] = true;
                    if (!input_errors[consumer].type) input_errors[consumer] = input_errors[i];
                }
                if (--predecessors[consumer] == 0)
                {
                    ready.push_back(consumer);
                }
            }
        }
        if (ready.size() != active.size()) frame_input_error_ = {kGraphDependencyCycle};
        active = std::move(ordered);
        frame_instances_.clear();
        frame_outputs_.clear();
        frame_storages_.clear();
        for (auto id : active)
        {
            const auto& job = *domain_->jobs_.find(id);
            auto& execution = *job.execution;
            auto reads = execution.instance.sampledTargets();
            for (const auto& input : execution.target_images)
                if (std::ranges::find(reads, input.target) == reads.end()) reads.push_back(input.target);
            frame_outputs_.push_back({id, &execution, execution.target, std::move(reads)});
            frame_instances_.push_back(&execution.instance);
            const auto& effect = execution.recipe.effect;
            auto& scene = *domain_->scenes_.find(job.content.scene);
            auto& view = *scene.views.find(job.content.view);
            for (const auto& dependency : effect.resource_dependencies)
            {
                const auto* factory = domain_->resources_.factory(dependency.declaration);
                const auto* ops = static_cast<const detail::StorageBindingOps*>(factory->storage->binding_ops);
                if (!ops || !ops->prepareFrame)
                {
                    continue;
                }
                auto* owner = view.storages.ownerFor(dependency.scope);
                const auto storage = owner ? owner->find(dependency.declaration) : ResourceInstanceId{};
                if (!storage.isNull())
                {
                    auto* native = domain_->resources_.resourceInstance(storage);
                    frame_storages_.push_back(
                        {native, ops, storage, ops->preparationRevision ? ops->preparationRevision(native) : nullptr}
                    );
                }
            }

        }
        frame_job_order_.clear();
        job_positions.clear();
        for (std::size_t index = 0; index < active.size(); ++index)
            job_positions.emplace(std::pair{active[index].index, active[index].gen}, index);
        std::vector<RenderJobId> ancestors;
        std::vector<RenderJobId> visited;
        for (std::uint32_t consumer = 0; consumer < active.size(); ++consumer)
        {
            ancestors = domain_->jobs_.find(active[consumer])->relations.predecessors;
            visited.clear();
            while (!ancestors.empty())
            {
                const auto id = ancestors.back();
                ancestors.pop_back();
                if (std::ranges::find(visited, id) != visited.end()) continue;
                visited.push_back(id);
                const auto ready = job_positions.find({id.index, id.gen});
                if (ready != job_positions.end())
                {
                    frame_job_order_.push_back({static_cast<std::uint32_t>(ready->second), consumer});
                    continue;
                }
                const auto& parents = domain_->jobs_.find(id)->relations.predecessors;
                ancestors.insert(ancestors.end(), parents.begin(), parents.end());
            }
        }
        std::ranges::sort(frame_storages_, {}, [](const auto& use) noexcept
            { return std::pair{use.identity.index, use.identity.gen}; });
        const auto duplicate = std::ranges::unique(frame_storages_, {}, &detail::StorageFrameUse::identity);
        frame_storages_.erase(duplicate.begin(), duplicate.end());
        active_frame_inputs_dirty_ = false;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        const auto rebuild_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - rebuild_started_at
        ).count();
        input_rebuilds_.fetch_add(1, std::memory_order_relaxed);
        input_rebuild_ns_.fetch_add(rebuild_ns, std::memory_order_relaxed);
        jobs_selected_.fetch_add(frame_outputs_.size(), std::memory_order_relaxed);
        dependency_edges_.fetch_add(frame_job_order_.size(), std::memory_order_relaxed);
#endif
    }

    std::uint32_t RenderServer::Impl::outputSlot(TargetId target, std::uint32_t frame_slot) const noexcept
    {
        if (std::ranges::find(frame_outputs_, target, &FrameOutput::target) != frame_outputs_.end()) return frame_slot;
        return publishedOutputSlot(target);
    }

    std::uint32_t RenderServer::Impl::publishedOutputSlot(TargetId target) const noexcept
    {
        const auto* published = domain_->job_outputs_.tryGet(target);
        const auto* offscreen = std::get_if<detail::OffscreenTarget>(domain_->targets_.find(target));
        if (!published || !offscreen) return UINT32_MAX;
        const bool same_identity = published->target == target && published->target_epoch == offscreen->state.epoch;
        if (!same_identity || !offscreen->matches(published->image_index, published->content_revision))
            return UINT32_MAX;
        return published->image_index;
    }

    void RenderServer::Impl::prefetchFrameOutputs(std::uint32_t slot) noexcept
    {
        target_readiness_scratch_.clear();
        for (auto id : running_jobs_)
        {
            auto& job = *domain_->jobs_.find(id);
            if (!job.readyToRun() || !jobTargetReady(job)) continue;
            if (!job.execution->programsReady() || !resourcesAvailable(job.execution->resources))
            {
                target_readiness_scratch_.push_back(id);
                continue;
            }
            const auto& target = *domain_->targets_.find(job.execution->target);
            if (const auto* surface = std::get_if<detail::SwapchainTarget>(&target))
            {
                const auto ready = surface->prefetched();
                if (!ready)
                {
                    job.last_run_error = ready.error();
                    job.control.rejectUnadmitted(ready.error());
                    active_frame_inputs_dirty_ = true;
                    continue;
                }
                if (!*ready) target_readiness_scratch_.push_back(id);
            }
            else if (!std::get<detail::OffscreenTarget>(target).writable(slot))
            {
                target_readiness_scratch_.push_back(id);
            }
        }
        if (target_readiness_scratch_ != unready_jobs_)
        {
            unready_jobs_.swap(target_readiness_scratch_);
            active_frame_inputs_dirty_ = true;
        }
    }

    bool RenderServer::Impl::resourcesAvailable(std::span<const ResourceInstanceId> resources) const noexcept
    {
        return std::ranges::none_of(resources, [&](auto id) { return domain_->resources_.hasExclusiveCommand(id); });
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
                const auto* sampled = std::get_if<detail::OffscreenTarget>(domain_->targets_.find(source));
                if (!sampled) return cxx::unexpected(RenderError{kRuntimeIdentity});
                if (std::ranges::find(frame_outputs_, source, &FrameOutput::target) != frame_outputs_.end()) continue;
                const auto selected = outputSlot(source, slot);
                if (selected == UINT32_MAX) return cxx::unexpected(RenderError{kRuntimeState});
                if (!sampled->readable(selected)) return false;
            }
            auto& target = *domain_->targets_.find(output.target);
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
                const auto image = targetOutput(*domain_->targets_.find(input.target)).frames[selected].image;
                execution.instance.bindImage(input.resource, slot, *image);
            }
        }
        return true;
    }

    void RenderServer::Impl::progressTargetFlights() noexcept
    {
        using NativeStep = detail::SwapchainTarget::ENativeStep;
        const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        for (auto& target : domain_->targets_)
        {
            auto* surface = std::get_if<detail::SwapchainTarget>(&target);
            if (!surface) continue;
            const auto& writer = surface->state;
            const bool borrowed = frame_run_count_ && std::ranges::any_of(frame_outputs_, [&](const auto& output)
            {
                return domain_->targets_.find(output.target) == &target;
            });
            const auto* job = domain_->jobs_.find(writer.writer);
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
        const auto& submissions = domain_->device_->frameWorkspace().submissions;
        const auto submitted_before = submissions.submittedBatches(slot);
        const auto accepted = co_await round_.operation->run(frame_instances_, frame_storages_, frame_job_order_);
        submissions_.fetch_add(submissions.submittedBatches(slot) - submitted_before, std::memory_order_relaxed);
        for (std::size_t i = 0; i < frame_outputs_.size(); ++i)
        {
            auto& output = frame_outputs_[i];
            auto* offscreen = std::get_if<detail::OffscreenTarget>(domain_->targets_.find(output.target));
            if (output.submitted) continue;
            const auto receipt = submissions.acceptedFrameUse(slot, *frame_instances_[i], target_frame_uses_);
            output.submitted = receipt.completed;
            for (auto source : output.reads)
            {
                auto* sampled = domain_->targets_.find(source);
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
            const auto* published = domain_->job_outputs_.tryGet(output.target);
            if (receipt.completed)
            {
                const auto revision = offscreen->publish(slot);
                const detail::JobOutput publication{output.target, slot, offscreen->state.epoch, revision};
                domain_->job_outputs_.insert(output.target, publication);
            }
            else if (published && published->image_index == slot)
            {
                domain_->job_outputs_.erase(output.target);
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
        const auto* job = domain_->jobs_.find(_request);
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

    void RenderServer::Impl::queryRenderDevice(
        const QueryRenderDevice::Request&, QueryRenderDevice::Response& response
    ) noexcept
    {
        const auto& session = domain_->device_->session();
        const auto& description = session.description();
        response = {
            {},
            description.graphicsAPI == nri::GraphicsAPI::D3D12 ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN,
            session.resourceCapabilities(),
            description.dimensions.attachmentMaxDim,
            static_cast<std::uint32_t>(session.queueCount()),
            domain_->device_->frameProgress().frameCount()
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
            auto borrowed = domain_->resources_.acquireCommand(id, command, exclusive);
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
        const auto* effect = domain_->effects_.find(entry.effect(request));
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
        const auto* entry = domain_->command_registry_.findCommand(id);
        if (!entry || route.request_id.isNull())
        {
            response_outbox_.emplaceResponse<AckResponse>(route, AckResponse{RenderError{kCommandContract}});
        }
        else if (received.message().typeHash() != entry->description->request.hash())
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kCommandContract});
        }
        else if (domain_->device_->session().terminalError().type != 0 && !entry->description->failure_safe)
        {
            entry->make_error(response_outbox_.prepare(route), domain_->device_->session().terminalError());
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
        for (auto& job : domain_->jobs_)
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
            const bool pending_operations = upload_progress_pending_ || round_.operation ||
                !input_updates_.empty() || preparing_execution_;
            const bool pending_contexts = !server_context_.is_idle() || !domain_->device_->nativeIdle();
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
        if (auto drained = domain_->device_->session().drain(); !drained) failExecution(drained.error());
        domain_.reset();
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
        if (auto completed = domain_->device_->session().progressCompletions(); !completed)
        {
            failExecution(completed.error());
        }
        if (admitting && !stop_requested_.load())
        {
            progressTargetFlights();
        }
        progressed |= progressInputUpdates();
        if (admitting) progressed |= progressProgramPreparations();
        progressed |= progressFrame();
        progressUploads();
        progressed |= server_context_.run_ready(continuation_budget) != 0;
        progressed |= publishResponses(response_budget) != 0;
        return progressed || submissions_.load(std::memory_order_relaxed) != submitted_before;
    }

    void RenderServer::Impl::waitForService(std::uint64_t observed) const noexcept
    {
        // IPC, ManualContext and stop publish the same epoch. GPU completion has no host
        // notification, and IPC scans a bounded Link prefix, so both keep a finite revisit.
        const auto delay = domain_->device_->session().pendingGpuWork()
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
            domain_->command_registry_.activateHandlers(domain_->resources_);
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
        if (const auto terminal = domain_->device_->session().terminalError(); terminal.type) failExecution(terminal);
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
            discarded_preparations_.load(std::memory_order_relaxed)
        };
#else
        return std::nullopt;
#endif
    }

} // namespace lux::render
