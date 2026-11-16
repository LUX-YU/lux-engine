#include <exception>
#include <exec/variant_sender.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/execution/ResizeResources.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <map>
#include <ranges>
#include <set>

namespace lux::render
{
    RenderServer::Impl::Impl(RenderServerConfig value, ipc::IpcResponder input, execution::CpuScheduler scheduler)
        : config_(std::move(value)), cpu_scheduler_(std::move(scheduler)), responder_(std::move(input)),
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
        bindFrameworkCommand<UpdateAttachmentInputs, &Impl::updateAttachmentInputs>();
        bindFrameworkCommand<InvalidateHistory, &Impl::invalidateHistory>();
        bindFrameworkCommand<RemoveView, &Impl::removeView>();
        bindFrameworkCommand<CreateTarget, &Impl::createTarget>();
        bindFrameworkCommand<ResizeTarget, &Impl::resizeTarget>();
        bindFrameworkCommand<RemoveTarget, &Impl::removeTarget>();
        bindFrameworkCommand<SetTargetActive, &Impl::setTargetActive>();
        bindCreateEffectCommand();
        bindFrameworkCommand<RemoveEffect, &Impl::removeEffect>();
        bindFrameworkCommand<Attach, &Impl::attachEffect>();
        bindFrameworkCommand<Detach, &Impl::detachEffect>();
        bindFrameworkCommand<Stop, &Impl::stopCommand>();
        bindFrameworkCommand<QueryRenderStats, &Impl::queryRenderStats>();
        bindFrameworkCommand<QueryExecutionProgress, &Impl::queryExecutionProgress>();
        bindFrameworkCommand<ListCapabilities, &Impl::listCapabilities>();
        bindFrameworkCommand<ValidateCapabilityConfiguration, &Impl::validateCapabilityConfiguration>();
        bindFrameworkCommand<ListCommands, &Impl::listCommands>();
        bindFrameworkCommand<ListCommandBindings, &Impl::listCommandBindings>();
        bindFrameworkCommand<SelectCommandBinding, &Impl::selectCommandBinding>();
        bindSubmitFrameCommand();
        frame_inputs_.resize(static_cast<std::size_t>(config_.max_frames_ahead) + 1);
        submit_frame_command_ = *command_registry_.findCommand(commandDescriptor<SubmitFrame>());
        select_binding_command_ = *command_registry_.findCommand(commandDescriptor<SelectCommandBinding>());
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
        auto created_device = detail::DeviceSession::create(device_options, callbacks, config_.validation);
        if (!created_device)
        {
            return cxx::unexpected(created_device.error());
        }
        device_ = std::move(*created_device);
        if (config_.uploads)
        {
            auto created_uploads = detail::ResourceUploader::create(*device_, *config_.uploads, cpu_scheduler_);
            if (!created_uploads)
            {
                return cxx::unexpected(created_uploads.error());
            }
            uploads_ = std::move(*created_uploads);
        }
        auto created_frames = detail::FrameExecution::create(*device_, uploads_.get(), config_.frames_in_flight);
        if (!created_frames)
        {
            return cxx::unexpected(created_frames.error());
        }
        frames_ = std::move(*created_frames);
        resources_.emplace();
        global_storages_.emplace(*resources_, EResourceDomain::DEVICE);
        paged_instances_ = device_options.buffer_device_address;
        return {};
    }

    namespace
    {
        // One operation owns the input data moved to the CPU scheduler. Device and
        // registered feature declarations are borrowed: the Server's command scope
        // must be settled before either owner is destroyed.
        struct EffectCompileWork final
        {
            nri::Device& device;
            const nri::CoreInterface& core;
            detail::EnabledDeviceFeatures features;
            std::array<const detail::QueueProperties*, 3> queues;
            std::vector<ResourceDeclaration> resource_declarations;
            GraphEffectRequest request;
            std::vector<const FeatureEntry*> entries;
            std::size_t config_limit;
            std::vector<RenderEffect::ResourceDependency> resource_dependencies;

            [[nodiscard]] RenderResult<std::pair<RenderEffect, RenderGraph>> compile() && noexcept
            {
                return compileEffect(
                    device,
                    core,
                    features,
                    queues,
                    resource_declarations,
                    request,
                    entries,
                    config_limit,
                    std::move(resource_dependencies)
                );
            }
        };
    } // namespace

    exec::task<EffectCreated> RenderServer::Impl::adoptCompiledEffect(
        EffectCompileResult candidate,
        std::vector<RenderEffect::Implementation> implementations
    ) noexcept
    {
        if (!candidate)
        {
            co_return EffectCreated{candidate.error()};
        }

        auto [compiled_effect, graph] = std::move(*candidate);
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested())
        {
            co_return EffectCreated{RenderError{kCommandCancelled}};
        }

        auto programs = co_await compiled_effect.plan->preparePrograms(*device_, graph, cpu_scheduler_);
        if (!programs)
        {
            co_return EffectCreated{programs.error()};
        }
        if (stop.stop_requested())
        {
            co_return EffectCreated{RenderError{kCommandCancelled}};
        }

        compiled_effect.implementations = std::move(implementations);
        if (auto bindings = resolveFrameBindings(compiled_effect, graph); !bindings)
        {
            co_return EffectCreated{bindings.error()};
        }

        const auto effect = effects_.emplace(std::move(compiled_effect));
        compiled_graphs_.fetch_add(1, std::memory_order_relaxed);
        co_return EffectCreated{{}, effect};
    }

    auto RenderServer::Impl::compileGraphEffect(
        GraphEffectRequest&& input,
        RenderError initial_error,
        std::vector<RenderEffect::Implementation> implementations,
        std::vector<RenderEffect::ResourceDependency> resource_dependencies
    ) noexcept
    {
        std::vector<const FeatureEntry*> registrations;
        if (initial_error.type == 0 && input.definition.features.empty())
        {
            initial_error = {kFeatureContract};
        }
        registrations.reserve(input.definition.features.size());
        for (const auto& selection : input.definition.features)
        {
            if (initial_error.type != 0)
            {
                break;
            }
            const auto* found = features_.find(selection.feature);
            if (!found)
            {
                initial_error = {kRuntimeIdentity};
                break;
            }
            registrations.push_back(found);
        }

        std::vector<ResourceDeclaration> resource_declarations;
        resources_->forEachType(
            [&](ResourceTypeId id, const ResourceDefinition& type) noexcept
            {
                const auto* factory = resources_->factory(id);
                if (factory && factory->storage->binding_ops &&
                    static_cast<const detail::StorageBindingOps*>(factory->storage->binding_ops)->resolve)
                {
                    resource_declarations.push_back({id, type.type, type.domain});
                }
            }
        );
        std::ranges::sort(resource_declarations, {}, [](const ResourceDeclaration& type) { return type.name.name(); });
        EffectCompileWork work{
            device_->device(),
            device_->nriCore(),
            device_->enabledFeatures(),
            {
                &device_->queue(detail::EQueueRole::GRAPHICS).properties(),
                &device_->queue(detail::EQueueRole::COMPUTE).properties(),
                &device_->queue(detail::EQueueRole::TRANSFER).properties()
            },
            std::move(resource_declarations),
            std::move(input),
            std::move(registrations),
            config_.max_config_bytes,
            std::move(resource_dependencies)
        };

        // The CPU operation owns its request. Only the RenderThread adopts a compiled effect.
        auto compiled =
            stdexec::schedule(cpu_scheduler_) |
            stdexec::then([work = std::move(work)]() mutable noexcept { return std::move(work).compile(); }) |
            stdexec::continues_on(server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery)) |
            stdexec::let_value(
                [this, implementations = std::move(implementations)](EffectCompileResult& candidate) mutable noexcept
                {
                    return adoptCompiledEffect(std::move(candidate), std::move(implementations));
                }
            );
        auto invalid = stdexec::just(EffectCreated{initial_error});
        using VSource = exec::variant_sender<decltype(invalid), decltype(compiled)>;
        return initial_error.type != error::ErrorId{} ? VSource{std::move(invalid)} : VSource{std::move(compiled)};
    }

    auto RenderServer::Impl::createCapabilityEffect(CreateEffect::Request&& input) noexcept
    {
        GraphEffectRequest graph{input.output};
        std::vector<RenderEffect::ResourceDependency> resource_dependencies;
        auto built = buildCapabilityGraph(input, graph, resource_dependencies);
        std::vector<RenderEffect::Implementation> implementations;
        for (const auto& contribution : input.composition.contributions)
        {
            implementations.push_back({contribution.implementation_id, contribution.key, {}});
        }
        return compileGraphEffect(
            std::move(graph),
            built ? RenderError{} : built.error(),
            std::move(implementations),
            std::move(resource_dependencies)
        );
    }

    void RenderServer::Impl::bindCreateEffectCommand() noexcept
    {
        bindFrameworkCommand<CreateEffect, &Impl::createCapabilityEffect>();
    }

    void RenderServer::Impl::removeEffect(
        const RemoveEffect::Request& _request,
        RemoveEffect::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        auto* effect = effects_.find(input->effect);
        if (!effect)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        // Effect deletion must not silently destroy other Views' associations. The creator
        // removes it only after every borrowing Attachment has explicitly left.
        if (!effect->attachments.empty())
        {
            output_response.error = RenderError{kRuntimeState};
            return;
        }
        effects_.erase(input->effect);
        output_response = {};
    }

    RenderResult<EffectAttachment> RenderServer::Impl::prepareAttachment(
        RenderEffect& effect,
        detail::TargetOutput output,
        ResourceSet& scene,
        RenderView& view,
        const AttachmentRequest& input
    ) noexcept
    {
        const auto target_images = std::span(input.target_images);
        const auto& plan = *effect.plan;
        auto dependencies = resourceCreateContext();
        const std::array scope_sets{&*global_storages_, &scene, &view.storages};
        std::vector<ResourceInstanceId> dependency_storages;
        dependency_storages.reserve(effect.resource_dependencies.size());
        for (const auto& dependency : effect.resource_dependencies)
        {
            auto instance = ensureResource(
                *scope_sets[static_cast<std::size_t>(dependency.scope)],
                dependency.declaration
            );
            if (!instance)
            {
                return cxx::unexpected(instance.error());
            }
            dependency_storages.push_back(*instance);
        }
        EffectAttachment candidate;
        candidate.instance =
            std::make_unique<detail::RenderGraphInstance>(*device_, frames_->frameCount(), plan, output);
        candidate.target_images.reserve(target_images.size());
        for (const auto& input : target_images)
        {
            const auto* source = targets_.find(input.target);
            if (!source || !source->description.sampled || source->surface())
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            const auto resource =
                std::ranges::find(plan.resources(), input.resource_key, &detail::CompiledResource::key);
            if (resource == plan.resources().end())
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
            const auto index = static_cast<std::uint32_t>(resource - plan.resources().begin());
            const GraphResourceId id{index};
            const auto* image = std::get_if<GraphImageDesc>(&resource->description);
            const bool duplicate = std::ranges::any_of(
                candidate.target_images,
                [id](const auto& imported) noexcept { return imported.resource == id; }
            );
            if (duplicate || !image || !resource->memory_used ||
                resource->origin != EGraphOrigin::IMPORTED || !resource->sampled_target_input)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
            // Static imported-resource structure. This belongs in Graph compilation
            // once the compiled import contract is available to Attachment creation.
            if (image->layers != 1 || image->mips != 1)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            if (source->resizing)
            {
                return cxx::unexpected(RenderError{kDeviceBusy});
            }

            const auto images = source->output();
            // Dynamic properties can change when a Target is resized. Target::description
            // is already validated as sampled above, so do not test it a second time.
            if (images.description.samples != image->samples || images.description.format != image->format)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }

            candidate.target_images.push_back({input.target, id});
            for (std::uint32_t slot = 0; slot < frames_->frameCount(); ++slot)
            {
                candidate.instance->bindImage(id, slot, *images.frames[slot].image);
            }
        }
        candidate.storage_inputs.reserve(plan.resources().size());
        std::vector<ResourceInstanceId> resolved(plan.resources().size());
        for (std::size_t dependency_index = 0; dependency_index < effect.resource_dependencies.size();
             ++dependency_index)
        {
            const auto& dependency = effect.resource_dependencies[dependency_index];
            const auto* factory = resources_->factory(dependency.declaration);
            const auto id = dependency_storages[dependency_index];
            for (const auto& argument : dependency.bindings)
            {
                const auto index = argument.resource.index;
                const auto type = argument.type();
                if (type != factory->descriptor->binding_type)
                {
                    return cxx::unexpected(RenderError{kStorageContract});
                }
                const auto* value = argument.data();
                const auto* ops = static_cast<const detail::StorageBindingOps*>(factory->storage->binding_ops);
                resolved[index] = id;
                if (auto bound =
                        candidate
                            .bindStorage(index, *ops, resources_->resourceInstance(id), value, frames_->frameCount());
                    !bound)
                {
                    return cxx::unexpected(bound.error());
                }
            }
        }
        for (const auto& binding : effect.bindings)
        {
            auto created = binding.create(
                *device_,
                *frames_,
                *resources_,
                *candidate.instance,
                device_->programs(),
                resolved,
                binding.configuration.data()
            );
            if (!created)
            {
                return cxx::unexpected(created.error());
            }
            created->definition = &binding;
            created->resolved_inputs.resize(binding.inputs.size());
            for (std::uint32_t slot = 0; slot < frames_->frameCount(); ++slot)
            {
                for (const auto& update : created->initial_resources)
                {
                    candidate.instance->setResource(update.resource.index, slot, update.input);
                }
            }
            created->initial_resources = {};
            candidate.feature_bindings.push_back(std::move(*created));
        }
        for (std::uint32_t input = 0; input < candidate.instance->samplerInputCount(); ++input)
        {
            if (!candidate.instance->usesSampler(input))
            {
                continue;
            }
            auto sampler = device_->descriptors().sampler(effect.samplers[input]);
            if (!sampler)
            {
                return cxx::unexpected(sampler.error());
            }
            if (auto bound =
                    candidate.instance->bindSampler(input, **sampler, effect.samplers[input].comparison.has_value());
                !bound)
            {
                return cxx::unexpected(bound.error());
            }
        }
        if (auto ready = candidate.instance->initialize(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (input.parameter_type == cxx::make_basic_type_info<CameraInput>())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto parameters = input.parameters.empty() && !effect.view_projections.empty()
            ? candidate.instance->instanceParameters() : std::span<const std::byte>{input.parameters};
        if (auto values =
                candidate.instance
                    ->setInputs(parameters, input.conditions, input.parameter_type, effect.parameter_projections);
            !values)
        {
            return cxx::unexpected(values.error());
        }
        candidate.instance->setViewport(input.viewport);
        for (const auto& projection : effect.view_projections)
        {
            auto applied = candidate.instance->setParameterInput(std::as_bytes(std::span{&view.camera, 1}), projection);
            if (!applied) return cxx::unexpected(applied.error());
        }
        if (!effect.view_projections.empty())
        {
            const auto& viewport = view.camera.viewport;
            candidate.instance->setViewport({viewport[0], viewport[1], viewport[2], viewport[3]});
        }
        candidate.camera_revision = view.camera_revision;
        return candidate;
    }

    bool RenderServer::Impl::canChangeAttachments(std::span<const AttachmentId> ids) const noexcept
    {
        return std::ranges::all_of(
            ids,
            [&](auto id)
            {
                const auto& attachment = *attachments_.find(id);
                auto* instance = attachment.instance.get();
                const bool target_resizing = targets_.find(attachment.target)->resizing ||
                                             std::ranges::any_of(
                                                 attachment.target_images,
                                                 [&](const auto& input) { return targets_.find(input.target)->resizing; }
                                             );
                return !target_resizing && frames_->canChangeResources({&instance, 1});
            }
        );
    }

    void RenderServer::Impl::detachAttachment(AttachmentId id) noexcept
    {
        auto& attachment = *attachments_.find(id);

        auto& view = *scenes_.find(attachment.scene)->views.find(attachment.view);
        auto& effect = *effects_.find(attachment.effect);
        auto& target = *targets_.find(attachment.target);
        const auto last_view = view.attachments.back();
        view.attachments[attachment.view_position] = last_view;
        attachments_.find(last_view)->view_position = attachment.view_position;
        view.attachments.pop_back();

        const auto last_effect = effect.attachments.back();
        effect.attachments[attachment.effect_position] = last_effect;
        attachments_.find(last_effect)->effect_position = attachment.effect_position;
        effect.attachments.pop_back();
        for (const auto& input : attachment.target_images)
        {
            std::erase(targets_.find(input.target)->consumers, id);
        }
        target.attachment = {};
        active_targets_.erase(attachment.target);
        active_frame_inputs_dirty_ = true;
        attachments_.erase(id);
    }

    void RenderServer::Impl::attachEffect(const Attach::Request& _request, Attach::Response& output_response) noexcept
    {

        const auto& scene_id = _request.scene;
        const auto& view_id = _request.view;
        const auto& target_id = _request.target;
        const auto& effect_id = _request.effect;
        auto* scene = scenes_.find(scene_id);
        auto* view = scene ? scene->views.find(view_id) : nullptr;
        auto* target = targets_.find(target_id);
        auto* effect = effects_.find(effect_id);
        const bool invalid = !view || !target || !effect;
        if (invalid)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (target->resizing)
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        if (target->surface() && !target->surface()->available())
        {
            output_response.error = RenderError{kRuntimeState};
            return;
        }
        if (target->surface() && effect->plan->output() == kGraphInvalidIndex)
        {
            output_response.error = RenderError{kRuntimeUnsupported};
            return;
        }
        const bool target_mismatch = !target->attachment.isNull() && target->attachment != _request.replaces;
        if (target_mismatch)
        {
            output_response.error = RenderError{kRuntimeState};
            return;
        }
        if (!_request.replaces.isNull())
        {
            const auto* previous = attachments_.find(_request.replaces);
            if (!previous)
            {
                output_response.error = RenderError{kRuntimeIdentity};
                return;
            }
            const bool different_view = previous->scene != scene_id || previous->view != view_id;
            if (different_view || !canChangeAttachments({&_request.replaces, 1}))
            {
                output_response.error = RenderError{different_view ? kRuntimeIdentity : kDeviceBusy};
                return;
            }
        }
        if (auto valid = _request.viewport.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        if (targetDependencyCycle(target_id, _request.target_images, _request.replaces))
        {
            output_response.error = RenderError{kCommandContract};
            return;
        }
        auto prepared =
            prepareAttachment(*effect, target->output(), scene->storages, *view, _request);
        if (!prepared)
        {
            output_response.error = prepared.error();
            return;
        }
        prepared->effect = effect_id;
        prepared->scene = scene_id;
        prepared->view = view_id;
        prepared->target = target_id;
        if (!_request.replaces.isNull())
        {
            detachAttachment(_request.replaces);
        }
        const auto id = attachments_.emplace(std::move(*prepared));
        auto& attachment = *attachments_.find(id);
        attachment.view_position = view->attachments.size();
        attachment.effect_position = effect->attachments.size();
        view->attachments.push_back(id);
        effect->attachments.push_back(id);
        target->attachment = id;
        for (const auto& input : attachment.target_images)
        {
            auto& consumers = targets_.find(input.target)->consumers;
            if (std::ranges::find(consumers, id) == consumers.end())
            {
                consumers.push_back(id);
            }
        }
        output_response = {};
        output_response.attachment = id;
        if (target->enabled)
        {
            active_targets_.emplace(target_id);
        }
        active_frame_inputs_dirty_ = true;
        storages_created_.store(resources_->creations(), std::memory_order_relaxed);
        return;
    }

    bool RenderServer::Impl::targetDependencyCycle(
        TargetId destination,
        std::span<const TargetImageInput> inputs,
        AttachmentId replaced
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
            const auto* target = targets_.find(id);
            const auto* attachment =
                target && target->attachment != replaced ? attachments_.find(target->attachment) : nullptr;
            if (attachment)
            {
                for (const auto& input : attachment->target_images)
                {
                    pending.push_back(input.target);
                }
            }
        }
        return false;
    }

    void RenderServer::Impl::createScene(
        const CreateScene::Request& _request,
        CreateScene::Response& output_response
    ) noexcept
    {
        output_response = SceneCreated{{}, scenes_.emplace(*resources_, *global_storages_)};
    }

    void RenderServer::Impl::removeScene(
        const RemoveScene::Request& _request,
        RemoveScene::Response& output_response
    ) noexcept
    {
        const auto* input = &_request;
        const auto scene_id = input->scene;
        auto* scene = scenes_.find(scene_id);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (scene->storages.hasCommands())
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        for (auto& view : scene->views)
        {
            if (view.storages.hasCommands() || !canChangeAttachments(view.attachments))
            {
                output_response.error = RenderError{kDeviceBusy};
                return;
            }
        }
        for (auto& view : scene->views)
        {
            while (!view.attachments.empty())
            {
                detachAttachment(view.attachments.back());
            }
        }
        scenes_.erase(scene_id);
        output_response = {};
    }

    void
    RenderServer::Impl::createView(const CreateView::Request& _request, CreateView::Response& output_response) noexcept
    {
        const auto* input = &_request;
        auto* scene = scenes_.find(input->scene);
        if (!scene)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        output_response = ViewCreated{{}, scene->views.emplace(*resources_, scene->storages)};
    }

    void RenderServer::Impl::updateAttachmentInputs(
        const UpdateAttachmentInputs::Request& input,
        UpdateAttachmentInputs::Response& output_response
    ) noexcept
    {
        auto* attachment = attachments_.find(input.attachment);
        if (input.parameter_type == cxx::make_basic_type_info<CameraInput>())
        {
            output_response.error = RenderError{kCommandContract};
            return;
        }
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (auto valid = input.viewport.validate(); !valid)
        {
            output_response.error = valid.error();
            return;
        }
        // Native validates the complete input size before changing any parameter bytes.
        const auto& effect = *effects_.find(attachment->effect);
        if (auto applied =
                attachment->instance
                    ->setInputs(input.parameters, input.conditions, input.parameter_type, effect.parameter_projections);
            !applied)
        {
            output_response.error = applied.error();
            return;
        }
        attachment->instance->setViewport(input.viewport);
        attachment->camera_revision = 0; // Reproject View-owned bytes before the next preparation.
        output_response = {};
    }

    void RenderServer::Impl::invalidateHistory(
        const InvalidateHistory::Request& input,
        InvalidateHistory::Response& output
    ) noexcept
    {
        auto* attachment = attachments_.find(input.attachment);
        if (!attachment)
        {
            output.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (auto invalidated = attachment->instance->invalidateHistory(input.previous); !invalidated)
        {
            output.error = invalidated.error();
            return;
        }
        output = {};
    }

    void
    RenderServer::Impl::removeView(const RemoveView::Request& _request, RemoveView::Response& output_response) noexcept
    {
        const auto* input = &_request;
        auto* scene = scenes_.find(input->scene);
        auto* view = scene ? scene->views.find(input->view) : nullptr;
        if (!view)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (view->storages.hasCommands() || !canChangeAttachments(view->attachments))
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        while (!view->attachments.empty())
        {
            detachAttachment(view->attachments.back());
        }
        scene->views.erase(input->view);
        output_response = {};
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
                detail::SurfaceTarget::create(*device_, input.description, *input.surface, frames_->frameCount());
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            id = targets_.emplace(RenderTarget::VPhysicalTarget{std::move(*physical)});
        }
        else
        {
            auto physical = detail::OffscreenTarget::create(*device_, input.description, frames_->frameCount());
            if (!physical)
            {
                response.error = physical.error();
                return;
            }
            id = targets_.emplace(RenderTarget::VPhysicalTarget{std::move(*physical)});
        }
        response = TargetCreated{{}, id, 1, targets_.find(id)->description, true};
    }

    RenderResult<RenderServer::Impl::ResizeCandidates> RenderServer::Impl::prepareTargetResize(
        TargetId target,
        detail::TargetOutput output,
        std::span<const AttachmentId> affected
    ) noexcept
    {
        ResizeCandidates candidates;
        candidates.reserve(affected.size());
        for (auto id : affected)
        {
            auto& attachment = *attachments_.find(id);
            std::vector<detail::RenderGraphInstance::ImageReplacement> inputs;
            for (const auto& image : attachment.target_images)
            {
                if (image.target == target)
                {
                    inputs.push_back({image.resource, output.frames});
                }
            }
            auto changed = attachment.instance->prepareResize(
                attachment.target == target ? output : targets_.find(attachment.target)->output(),
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

    exec::task<TargetCreated> RenderServer::Impl::resizeTarget(ResizeTarget::Request&& request) noexcept
    {
        const auto input = std::move(request);
        const auto response = [this, id = input.target](RenderError error = {}) noexcept
        {
            const auto* current = targets_.find(id);
            if (!current)
            {
                return TargetCreated{error};
            }
            const bool available = !current->surface() || current->surface()->available();
            return TargetCreated{error, id, current->epoch, current->description, available};
        };
        if (auto valid = input.description.validate(); !valid)
        {
            co_return response(valid.error());
        }
        auto* target = targets_.find(input.target);
        if (!target)
        {
            co_return response(RenderError{kRuntimeIdentity});
        }
        const bool different_kind = input.description.kind != target->description.kind;
        const auto maximum = device_->description().dimensions.attachmentMaxDim;
        const bool oversized = input.description.width > maximum || input.description.height > maximum;
        const bool removes_sampling = !input.description.sampled && !target->consumers.empty();
        if (different_kind || oversized || removes_sampling)
        {
            co_return response(RenderError{kCommandContract});
        }
        if (target->resizing) co_return response(RenderError{kDeviceBusy});
        const auto* surface_output = target->surface();
        const bool unchanged = input.description == target->description;
        const bool healthy = !surface_output || !surface_output->needsRecreation();
        if (unchanged && healthy) co_return response();
        auto affected = target->consumers;
        if (!target->attachment.isNull())
        {
            affected.push_back(target->attachment);
            const auto& attachment = *attachments_.find(target->attachment);
            if (!effects_.find(attachment.effect)->plan->compatible(input.description))
            {
                co_return response(RenderError{kDeviceArgument});
            }
        }
        if (!canChangeAttachments(affected))
        {
            co_return response(RenderError{kDeviceBusy});
        }
        if (target->epoch == UINT64_MAX)
        {
            co_return response(RenderError{kRuntimeCapacity});
        }
        std::vector<detail::RenderGraphInstance*> instances;
        std::vector<TargetId> paused;
        for (auto id : affected)
        {
            const auto& attachment = *attachments_.find(id);
            instances.push_back(attachment.instance.get());
            paused.push_back(attachment.target);
            active_targets_.erase(attachment.target);
        }
        target->resizing = true;
        active_frame_inputs_dirty_ = true;
        cxx::scope_exit finish(
            [&]() noexcept
            {
                targets_.find(input.target)->resizing = false;
                for (auto id : paused)
                {
                    const auto& output = *targets_.find(id);
                    const bool available = !output.surface() || output.surface()->available();
                    if (output.enabled && !output.attachment.isNull() && available)
                    {
                        active_targets_.emplace(id);
                    }
                }
                active_frame_inputs_dirty_ = true;
            }
        );
        const auto accepted = frames_->acceptedUses(instances);
        RenderTarget::VPhysicalTarget physical;
        ResizeCandidates candidates;
        const bool surface = target->surface() != nullptr;
        if (!surface)
        {
            auto created = detail::OffscreenTarget::create(*device_, input.description, frames_->frameCount());
            if (!created)
            {
                co_return response(created.error());
            }
            physical = std::move(*created);
            auto prepared = prepareTargetResize(
                input.target,
                std::visit([](const auto& value) { return value->output(); }, physical),
                affected
            );
            if (!prepared)
            {
                co_return response(prepared.error());
            }
            candidates = std::move(*prepared);
        }
        // The captured values retain all accepted uses, independently of queue role aliases.
        if (auto waited = co_await device_->waitFrontiers(accepted); !waited)
        {
            co_return response(waited.error());
        }
        target = targets_.find(input.target);
        if (surface)
        {
            const auto window = target->surface()->surface();
            if (!target->attachment.isNull())
            {
                attachments_.find(target->attachment)->instance->releaseOutputReferences();
            }
            auto closed = co_await target->surface()->close(accepted);
            if (!closed)
            {
                co_return response(closed.error());
            }
            target = targets_.find(input.target);
            auto created = detail::SurfaceTarget::create(*device_, input.description, window, frames_->frameCount());
            if (!created)
            {
                co_return response(created.error());
            }
            physical = std::move(*created);
            auto prepared = prepareTargetResize(
                input.target,
                std::visit([](const auto& value) { return value->output(); }, physical),
                affected
            );
            if (!prepared)
            {
                co_return response(prepared.error());
            }
            candidates = std::move(*prepared);
        }
        // Every fallible GPU creation is finished. Publish all producer/consumer references without suspension.
        for (auto& [id, candidate] : candidates)
        {
            if (auto ready = attachments_.find(id)->instance->prepareResizeAdoption(candidate); !ready)
            {
                co_return response(ready.error());
            }
        }
        for (auto& [id, candidate] : candidates)
        {
            attachments_.find(id)->instance->commitResize(std::move(candidate));
        }
        target = targets_.find(input.target);
        target->physical = std::move(physical);
        target->description = target->output().description;
        ++target->epoch;
        // The reply carries the adopted extent. Resume only with matching inputs in a later complete frame.
        target->enabled = false;
        co_return response();
    }

    exec::task<AckResponse> RenderServer::Impl::removeTarget(RemoveTarget::Request&& input) noexcept
    {
        auto* target = targets_.find(input);
        if (!target)
        {
            co_return AckResponse{RenderError{kRuntimeIdentity}};
        }
        if (target->resizing)
        {
            co_return AckResponse{RenderError{kDeviceBusy}};
        }
        if (!target->consumers.empty())
        {
            co_return AckResponse{RenderError{kRuntimeState}};
        }
        auto accepted = device_->makeFrontiers();
        // A detached target no longer has its instance in the active binding table. Keep the
        // conservative snapshot for that path; attached owners can identify their exact uses.
        for (std::size_t q = 0; q < device_->queueCount(); ++q)
        {
            accepted.accept(device_->submissionPoint(q, device_->physicalQueue(q).submitted()));
        }
        if (!target->attachment.isNull())
        {
            if (!canChangeAttachments({&target->attachment, 1}))
            {
                co_return AckResponse{RenderError{kDeviceBusy}};
            }
            auto* instance = attachments_.find(target->attachment)->instance.get();
            accepted = frames_->acceptedUses({&instance, 1});
            detachAttachment(target->attachment);
        }
        auto physical = std::move(target->physical);
        targets_.erase(input);
        if (auto* surface = std::get_if<std::unique_ptr<detail::SurfaceTarget>>(&physical))
        {
            auto closed = co_await (*surface)->close(accepted);
            if (!closed)
            {
                // The close completion follows actual SwapChain destruction, including DeviceLost.
                // Removal succeeded; report device failure separately instead of retaining a stale TargetId.
                failExecution(closed.error());
            }
        }
        co_return AckResponse{};
    }

    void RenderServer::Impl::setTargetActive(
        const SetTargetActive::Request& input,
        SetTargetActive::Response& response
    ) noexcept
    {
        auto* target = targets_.find(input.target);
        if (!target)
        {
            response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (target->resizing || (!target->attachment.isNull() && !canChangeAttachments({&target->attachment, 1})))
        {
            response.error = RenderError{kDeviceBusy};
            return;
        }
        if (input.active && input.epoch != target->epoch)
        {
            response.error = RenderError{kRuntimeState};
            return;
        }
        if (input.active && target->surface() && !target->surface()->available())
        {
            response.error = RenderError{kRuntimeState};
            return;
        }
        target->enabled = input.active;
        if (input.active && !target->attachment.isNull())
        {
            active_targets_.emplace(input.target);
        }
        else
        {
            active_targets_.erase(input.target);
        }
        active_frame_inputs_dirty_ = true;
        response = {};
    }

    void RenderServer::Impl::detachEffect(const Detach::Request& _request, Detach::Response& output_response) noexcept
    {
        const auto* attachment = attachments_.find(_request);
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        if (!canChangeAttachments({&_request, 1}))
        {
            output_response.error = RenderError{kDeviceBusy};
            return;
        }
        detachAttachment(_request);
        output_response = {};
    }

    void RenderServer::Impl::queryCommandIds(
        const QueryCommandIds::Request& names, QueryCommandIds::Response& response
    ) noexcept
    {
        command_registry_.queryCommandIds(names, response);
    }

    void RenderServer::Impl::listCommands(
        const ListCommands::Request& _request,
        ListCommands::Response& output_response
    ) noexcept
    {
        output_response = command_registry_.listCommands();
    }

    void RenderServer::Impl::listCommandBindings(
        const ListCommandBindings::Request& input,
        ListCommandBindings::Response& response
    ) noexcept
    {
        command_registry_.listCommandBindings(input, response);
    }

    void RenderServer::Impl::selectCommandBinding(
        const SelectCommandBinding::Request& input,
        SelectCommandBinding::Response& response
    ) noexcept
    {
        command_registry_.selectHandler(input, response, *resources_);
    }

    RenderResult<void> RenderServer::Impl::progressGpuCompletions() noexcept
    {
        return device_->progressCompletions();
    }

    void RenderServer::Impl::progressUploads() noexcept
    {
        if (uploads_ && !upload_progress_pending_ && uploads_->canProgressTransfers())
        {
            upload_progress_pending_ = true;
            const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
            stdexec::spawn(
                stdexec::starts_on(delivery, uploads_->submitReadyTransfers()) |
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

    RenderResult<void> RenderServer::Impl::applyViewCameras() noexcept
    {
        for (auto id : active_targets_.keys())
        {
            auto& attachment = *attachments_.find(targets_.find(id)->attachment);
            const auto& effect = *effects_.find(attachment.effect);
            const auto& view = *scenes_.find(attachment.scene)->views.find(attachment.view);
            if (attachment.camera_revision == view.camera_revision) continue;
            for (const auto& projection : effect.view_projections)
            {
                auto applied =
                    attachment.instance->setParameterInput(std::as_bytes(std::span{&view.camera, 1}), projection);
                if (!applied) return applied;
            }
            if (!effect.view_projections.empty())
            {
                const auto& viewport = view.camera.viewport;
                attachment.instance->setViewport({viewport[0], viewport[1], viewport[2], viewport[3]});
            }
            attachment.camera_revision = view.camera_revision;
        }
        return {};
    }

    void RenderServer::Impl::rebuildActiveFrameInputs() noexcept
    {
        // Semantic conflict order is stable. Fairness rotates ready physical queues, not
        // the order of writers of one imported resource.
        const auto keys = active_targets_.keys();
        std::vector<TargetId> active(keys.begin(), keys.end());
        std::ranges::sort(active, {}, &TargetId::index);
        std::map<std::uint32_t, std::size_t> positions;
        std::vector<std::vector<std::size_t>> successors(active.size());
        std::vector<std::size_t> predecessors(active.size());
        std::vector<bool> unavailable(active.size());
        for (std::size_t i = 0; i < active.size(); ++i)
        {
            positions.emplace(active[i].index, i);
        }
        for (std::size_t i = 0; i < active.size(); ++i)
        {
            const auto& attachment = *attachments_.find(targets_.find(active[i])->attachment);
            for (const auto& input : attachment.target_images)
            {
                const auto producer = positions.find(input.target.index);
                if (producer == positions.end())
                {
                    unavailable[i] = true; // No implicit previous-frame/history sampling.
                    continue;
                }
                successors[producer->second].push_back(i);
                ++predecessors[i];
            }
        }
        std::vector<std::size_t> ready;
        std::vector<TargetId> ordered;
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
            for (auto consumer : successors[i])
            {
                unavailable[consumer] = unavailable[consumer] || unavailable[i];
                if (--predecessors[consumer] == 0)
                {
                    ready.push_back(consumer);
                }
            }
        }
        active = std::move(ordered);
        frame_instances_.clear();
        frame_surfaces_.clear();
        frame_storages_.clear();
        frame_features_.clear();
        for (auto id : active)
        {
            auto& target = *targets_.find(id);
            if (auto* surface = target.surface())
            {
                frame_surfaces_.push_back(surface);
            }
            auto& attachment = *attachments_.find(target.attachment);
            frame_instances_.push_back(attachment.instance.get());
            const auto& effect = *effects_.find(attachment.effect);
            auto& scene = *scenes_.find(attachment.scene);
            auto& view = *scene.views.find(attachment.view);
            const std::array scopes{&*global_storages_, &scene.storages, &view.storages};
            for (const auto& dependency : effect.resource_dependencies)
            {
                const auto* factory = resources_->factory(dependency.declaration);
                const auto* ops = static_cast<const detail::StorageBindingOps*>(factory->storage->binding_ops);
                if (!ops || !ops->prepareFrame)
                {
                    continue;
                }
                const auto storage = scopes[static_cast<std::size_t>(dependency.scope)]->find(
                    dependency.declaration
                );
                if (!storage.isNull())
                {
                    auto* native = resources_->resourceInstance(storage);
                    frame_storages_.push_back(
                        {native, ops, storage, ops->preparationRevision ? ops->preparationRevision(native) : nullptr}
                    );
                }
            }
            for (auto& feature : attachment.feature_bindings)
            {
                frame_features_.emplace_back(attachment.instance.get(), &feature);
            }
        }
        std::ranges::sort(frame_storages_, {}, [](const auto& use) noexcept
            { return std::pair{use.identity.index, use.identity.gen}; });
        const auto duplicate = std::ranges::unique(frame_storages_, {}, &detail::StorageFrameUse::identity);
        frame_storages_.erase(duplicate.begin(), duplicate.end());
        active_frame_inputs_dirty_ = false;
    }

    RenderResult<bool> RenderServer::Impl::acquireFrameOutputs(std::uint32_t slot) noexcept
    {
        if (auto presented = presentFrameOutputs(); !presented)
        {
            return cxx::unexpected(presented.error());
        }
        for (auto* surface : frame_surfaces_)
        {
            const auto acquired = surface->acquire(slot);
            if (!acquired || !*acquired)
            {
                return acquired;
            }
        }
        return true;
    }

    RenderResult<std::uint32_t> RenderServer::Impl::presentFrameOutputs() noexcept
    {
        if (active_frame_inputs_dirty_)
        {
            rebuildActiveFrameInputs();
        }
        std::uint32_t count = 0;
        RenderError failure;
        for (auto* surface : frame_surfaces_)
        {
            const auto presented = surface->present();
            if (presented)
            {
                count += *presented;
            }
            else if (failure.type == 0)
            {
                failure = presented.error();
            }
        }
        presentations_.fetch_add(count, std::memory_order_relaxed);
        if (failure.type != 0)
        {
            return cxx::unexpected(failure);
        }
        return count;
    }

    exec::task<RenderResult<void>> RenderServer::Impl::submitCurrentFrame(std::uint32_t slot) noexcept
    {
        const auto accepted = co_await frames_->render(frame_instances_, frame_storages_, frame_features_);
        const auto presented = presentFrameOutputs();
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        submissions_.fetch_add(*accepted, std::memory_order_relaxed);
        if (!presented)
        {
            if (!frames_->inputConsumed(slot))
            {
                frames_->cancelSubmissions(presented.error());
            }
            co_return cxx::unexpected(presented.error());
        }
        co_return RenderResult<void>{};
    }

    void RenderServer::Impl::queryExecutionProgress(
        const QueryExecutionProgress::Request& _request,
        QueryExecutionProgress::Response& output_response
    ) noexcept
    {
        const auto* attachment = attachments_.find(_request);
        if (!attachment)
        {
            output_response.error = RenderError{kRuntimeIdentity};
            return;
        }
        output_response = {{}, attachment->instance->progress()};
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

    void RenderServer::Impl::stopCommand(const Stop::Request&, Stop::Response& response) noexcept
    {
        // Stop admission and drain accepted work. Cancellation during shutdown is
        // not itself an execution fault and should not emit execution_failed.
        stop();
        response = {};
    }

    void RenderServer::Impl::failExecution(RenderError error) noexcept
    {
        if (stop_requested_.exchange(true))
        {
            return;
        }
        for (std::size_t i = 0; i < frame_count_; ++i)
        {
            auto& frame = frame_inputs_[(first_frame_ + i) % frame_inputs_.size()];
            if (frame.error.type == 0)
            {
                frame.error = error;
            }
        }
        frames_->cancelSubmissions(error);
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
            const auto declaration = resources_->declarationOf(id);
            if (declaration.isNull()) return cxx::unexpected(RenderError{kStorageIdentity});
            const auto* location = resources_->commandBinding(declaration, command);
            const auto* selected = location ? command_registry_.binding(*location) : nullptr;
            if (!selected || !selected->call.invoke) return cxx::unexpected(RenderError{kCommandContract});
            auto borrowed = resources_->acquireCommand(id, selected->storage_type);
            if (!borrowed) return cxx::unexpected(borrowed.error());
            resource = std::move(*borrowed);
            return selected->call;
        }
        if (!entry.implementation)
        {
            const auto* binding = entry.selectedBinding();
            if (!binding || !binding->call.object)
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            return binding->call;
        }
        const auto* implementation = capability_registry_.find(entry.implementation(request));
        if (!implementation)
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        const auto method = std::ranges::find_if(
            implementation->command_bindings,
            [&](const auto& value) noexcept { return value.location.command == command; }
        );
        if (method == implementation->command_bindings.end())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        const auto* binding = command_registry_.binding(method->location);
        if (!binding || !binding->call.object)
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
        const auto* entry = command_registry_.findCommand(id);
        const bool is_wrong_frame_source = id == submit_frame_command_ && route.requester_id != frame_source_;
        const bool requires_frame_boundary =
            (entry && entry->description->requires_frame_boundary) || id == select_binding_command_;
        if (requires_frame_boundary && frame_count_ != 0)
        {
            // Retain the already received head message, not another command queue. Later IPC
            // messages cannot overtake it; GPU/completion progress continues until older frames settle.
            deferred_request_ = std::move(received);
            return;
        }
        if (!entry || route.request_id.isNull())
        {
            response_outbox_.emplaceResponse<AckResponse>(route, AckResponse{RenderError{kCommandContract}});
        }
        else if (received.message().typeHash() != entry->description->request.hash())
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kCommandContract});
        }
        else if (is_wrong_frame_source)
        {
            entry->make_error(response_outbox_.prepare(route), RenderError{kFrameSource});
        }
        else if (device_->terminalError().type != 0 && !entry->description->failure_safe && id != submit_frame_command_)
        {
            entry->make_error(response_outbox_.prepare(route), device_->terminalError());
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
                structural_command_running_ = requires_frame_boundary;
                CommandDispatchContext context{
                    received.message(),
                    route,
                    response_outbox_,
                    command_scope_,
                    server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery),
                    requires_frame_boundary ? &structural_command_running_ : nullptr,
                    std::move(resource)
                };
                // Async Handler construction consumes this exact call now; completion never
                // re-reads the capability directory or a selected Handler.
                call->invoke(call->object, context);
            }
        }
        commands_.fetch_add(1, std::memory_order_relaxed);
    }

    void RenderServer::Impl::shutdown(RenderError cause) noexcept
    {
        stop_requested_.store(true);
        if (frame_count_ != 0 && frame_inputs_[first_frame_].error.type == 0)
        {
            // A device/queue failure must reach the active frame; shutdown is not proof of cancellation.
            frame_inputs_[first_frame_].error = cause;
        }
        if (deferred_request_.active())
        {
            const ResponseRoute route{deferred_request_.requesterId(), RequestId{deferred_request_.correlation()}};
            const auto* entry = command_registry_.findCommand(commandIdFromTag(deferred_request_.tag()));
            entry->make_error(response_outbox_.prepare(route), cause);
            deferred_request_.reset();
        }
        command_scope_.close();
        command_scope_.request_stop();
        if (frames_)
        {
            frames_->cancelSubmissions(RenderError{kUploadStopped});
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
            }
        };

        auto join = stdexec::connect(command_scope_.join(), Joined{&joined});
        stdexec::start(join);
        publishResponses(32);
        // Keep RenderThread and GPU completion paths advancing until every accepted Sender settles.
        while (!joined.load(std::memory_order_acquire) || upload_progress_pending_ || !server_context_.is_idle())
        {
            // Adopt published CPU results while the Scope is draining.
            (void)server_context_.run_ready(128);
            publishResponses(32);
            if (device_)
            {
                auto collected = device_->progressCompletions();
                if (frames_)
                {
                    if (uploads_) uploads_->dispatchWrites();
                    frames_->cancelSubmissions(RenderError{kUploadStopped});
                }
                // Stopped upload continuations must run after GPU/write-admission callbacks.
                (void)server_context_.run_ready(128);
                progressUploads();
                if (!collected && !device_->deviceLost() && collected.error().type != kOutcomeUnknown)
                {
                    std::terminate();
                }
                progressFrame();
            }
            // Settle frame terminals before testing Scope completion again.
            (void)server_context_.run_ready(128);
            publishResponses(32);
            if (!joined.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }
        while (!server_context_.is_idle())
        {
            std::this_thread::yield();
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
        while (!response_outbox_.empty())
        {
            const auto observed = ipc::IpcCore::instance().workEpoch();
            if (publishResponses(SIZE_MAX) == 0)
            {
                ipc::IpcCore::instance().waitUntil(
                    observed,
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(1)
                );
            }
        }
        responder_.reset();
        ipc::IpcCore::instance().notifyWork(); // Wake bootstrap/reply waiters after CLOSED is published.
        // The active set is an execution index, not the attachment owner table.
        // Inactive targets retain attachments and must be detached too.
        // detachAttachment updates relationships but never erases a RenderTarget.
        for (auto& target : targets_)
        {
            if (!target.attachment.isNull())
            {
                detachAttachment(target.attachment);
            }
        }
        // End plan owners while physical cleanup is still admitted. The Session closes only
        // after Scene/Global Storage and every other producer have transferred their resources.
        effects_.clear();
        targets_.clear();
        scenes_.clear();
        command_registry_.clear(); // All operations ended; the borrowed resource directory is still alive.
        if (global_storages_)
        {
            global_storages_->clear();
        }
        global_storages_.reset();
        resources_.reset();
        // Frame callback nodes and Staging outlive all accepted GPU submissions.
        if (auto settled = device_->drain();
            !settled && !device_->deviceLost() && settled.error().type != kOutcomeUnknown)
        {
            std::terminate();
        }
        frames_.reset();
        uploads_.reset();
        device_.reset();
        (void)server_context_.close();
        running_.store(false);
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

    RenderResult<void> RenderServer::Impl::run() noexcept
    {
        if (has_started_.exchange(true))
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (stop_requested_.load())
        {
            shutdown();
            return {};
        }
        command_registry_.activateHandlers(*resources_);
        running_.store(true);
        RenderResult<void> result;
        auto& notifications = ipc::IpcCore::instance();
        while (!stop_requested_.load())
        {
            // Snapshot precedes every readiness check. A publication in the check/wait gap
            // changes the epoch, so WaitOnAddress/futex cannot sleep through that publication.
            const auto observed = notifications.workEpoch();
            const auto submitted_before = submissions_.load(std::memory_order_relaxed);
            // Safe point 1: adopt already-published CPU results before accepting more commands.
            bool progressed = server_context_.run_ready(128) != 0;
            // A structural request is admitted between complete frames. Its asynchronous terminal
            // opens the boundary again; this does not block GPU or stdexec completion progress.
            if (deferred_request_.active() && frame_count_ == 0 && !structural_command_running_)
            {
                auto request = std::move(deferred_request_);
                deferred_request_.reset();
                dispatchRequest(std::move(request));
                progressed = true;
            }
            for (std::uint32_t i = 0; i < 32; ++i)
            {
                const bool can_receive = !stop_requested_.load() && !deferred_request_.active() &&
                                         !structural_command_running_;
                if (!can_receive)
                {
                    break;
                }
                ipc::ReceivedRequest request;
                if (responder_.tryReceive(request) == ipc::EIpcReceive::RECEIVED)
                {
                    dispatchRequest(std::move(request));
                    progressed = true;
                }
            }
            if (stop_requested_.load())
            {
                break;
            }
            progressed |= publishResponses(32) != 0;
            if (auto observed_gpu = progressGpuCompletions(); !observed_gpu)
            {
                result = cxx::unexpected(observed_gpu.error());
                failExecution(observed_gpu.error());
            }
            if (uploads_) uploads_->dispatchWrites();
            // Safe point 2: GPU callbacks/write admission can schedule continuations required
            // by this frame. Count their progress before deciding whether the loop may wait.
            progressed |= server_context_.run_ready(128) != 0;
            progressUploads();
            // Staging availability resumes a Task through the same scheduler. Let that continuation
            // consume its admitted range before frame preparation closes background staging again;
            // otherwise continuously submitted frames can wake and immediately requeue it forever.
            progressed |= server_context_.run_ready(128) != 0;
            progressFrame();
            // A submitted frame may have returned its CPU input before a present-fence slot
            // becomes reusable. Continue presentation even when no new input frame is queued.
            if (frame_count_ == 0)
            {
                if (auto presented = presentFrameOutputs(); presented)
                {
                    progressed |= *presented != 0;
                }
                else
                {
                    result = cxx::unexpected(presented.error());
                    failExecution(presented.error());
                }
            }
            // Safe point 3: publish terminal frame/command results produced by this iteration.
            progressed |= server_context_.run_ready(128) != 0;
            progressed |= publishResponses(32) != 0;
            progressed |= submissions_.load(std::memory_order_relaxed) != submitted_before;
            ticks_.fetch_add(1, std::memory_order_relaxed);
            if (!progressed && !stop_requested_.load())
            {
                // A publisher may be counted before its node is visible. Its eventual wakeup
                // changes this epoch; spinning on ManualContext::pending() would delay other producers.
                if (notifications.workEpoch() != observed)
                {
                    continue;
                }
                if (device_->pendingGpuWork() && device_->terminalError().type == 0)
                {
                    if (auto advanced = frames_->waitForProgress(config_.gpu_wait_ns); !advanced)
                    {
                        result = cxx::unexpected(advanced.error());
                        failExecution(advanced.error());
                    }
                }
                else
                {
                    // Receive only inspected a bounded prefix of Links. Preserve its finite revisit.
                    notifications.waitUntil(observed, std::chrono::steady_clock::now() + std::chrono::milliseconds(1));
                }
            }
        }
        if (device_->terminalError().type != 0)
        {
            result = cxx::unexpected(device_->terminalError());
        }
        shutdown(result ? RenderError{kCommandCancelled} : result.error());
        return result;
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

} // namespace lux::render
