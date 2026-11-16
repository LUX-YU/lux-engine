#include <exec/create.hpp>
#include <lux/engine/render/resource/view/CameraInputHandler.hpp>
#include <limits>
#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    auto RenderServer::Impl::submitFrame(SubmitFrame::Request&& input) noexcept
    {
        // The library owns the connected state. Impl retains only the accepted frame's inputs
        // and a terminal receiver borrow; it never stores an operation deleter or a second Scope.
        return exec::create<stdexec::set_value_t(FrameCompletion)>(
            [this, input](auto& context) noexcept
            {
                using Context = std::remove_reference_t<decltype(context)>;
                admitFrame(
                    input,
                    &context,
                    [](void* value, FrameCompletion result) noexcept
                    {
                        auto& context = *static_cast<Context*>(value);
                        stdexec::set_value(std::move(context.receiver), std::move(result));
                    }
                );
                // Admission may complete synchronously. No access to context after publication.
            }
        );
    }

    void RenderServer::Impl::bindSubmitFrameCommand() noexcept
    {
        bindFrameworkCommand<SubmitFrame, &Impl::submitFrame>();
    }

    void RenderServer::Impl::admitFrame(
        RenderFrame input,
        void* receiver,
        void (*complete)(void*, FrameCompletion) noexcept
    ) noexcept
    {
        const auto reject = [&](RenderError error, std::uint32_t entry = UINT32_MAX) noexcept
        { complete(receiver, {error, input.sequence, EFrameOutcome::REJECTED, 0, frame_resynchronization_, entry}); };
        if (stop_requested_.load())
        {
            reject(RenderError{kCommandCancelled});
            return;
        }
        if (const auto rejection = device_->terminalError(); rejection.type != 0)
        {
            reject(rejection);
            return;
        }
        if (input.sequence == 0 || input.sequence <= last_frame_sequence_)
        {
            reject(RenderError{kFrameContract});
            return;
        }
        if (frame_count_ == frame_inputs_.size())
        {
            reject(RenderError{kRuntimeCapacity});
            return;
        }
        if (frame_resynchronization_ && !input.resynchronize)
        {
            reject(RenderError{kFrameResynchronization});
            return;
        }
        auto& frame = frame_inputs_[(first_frame_ + frame_count_) % frame_inputs_.size()];
        std::size_t covered = 0;
        for (const auto phase : input.phases)
        {
            const bool is_invalid_phase =
                phase.first != covered || phase.count == 0 || phase.count > input.entries.size() - covered;
            if (is_invalid_phase)
            {
                reject(RenderError{kFrameContract});
                return;
            }
            covered += phase.count;
        }
        if (covered != input.entries.size() || input.entries.size() > UINT32_MAX)
        {
            reject(RenderError{kFrameContract});
            return;
        }
        const FrameDataView data{input.blocks};
        for (std::uint32_t index = 0; index < input.entries.size(); ++index)
        {
            const auto& entry = input.entries[index];
            const auto binding = resolveFrameData(entry.destination);
            const bool is_missing_binding = !binding ||
                (binding->call->handler_type.isValid() && (!binding->call->handler || !binding->call->handler->object));
            if (is_missing_binding)
            {
                reject(RenderError{kRuntimeIdentity}, index);
                return;
            }
            const auto& call = *binding->call;
            const auto root = data.read(entry.payload);
            const bool is_invalid_root = !root || entry.payload.count != call.root_size ||
                                         reinterpret_cast<std::uintptr_t>(root->data()) % call.root_alignment != 0;
            if (is_invalid_root)
            {
                reject(RenderError{kFrameContract}, index);
                return;
            }
        }
        frame.input = input;
        frame.stage = FrameRecord::EStage::QUEUED;
        frame.phase = frame.dispatched = frame.pending = 0;
        frame.failed_entry = UINT32_MAX;
        frame.outcome_unknown = false;
        frame.slot = UINT32_MAX;
        frame.error = {};
        frame.receiver = receiver;
        frame.complete = complete;
        last_frame_sequence_ = input.sequence;
        ++frame_count_;
    }

    RenderResult<BoundFrameData> RenderServer::Impl::resolveFrameData(const VFrameDataDestination& selected) noexcept
    {
        if (const auto* resource = std::get_if<ResourceFrameDestination>(&selected))
        {
            const auto* factory = resources_->factory(resources_->declarationOf(resource->resource));
            const bool is_invalid_input = !factory || resource->input >= factory->frame_calls.size();
            if (is_invalid_input) return cxx::unexpected(RenderError{kRuntimeIdentity});
            const auto& call = factory->frame_calls[resource->input];
            auto target = call.resolve_storage(*resources_, resource->resource);
            if (!target) return cxx::unexpected(target.error());
            return BoundFrameData{&call, VFrameDataTarget{*target}};
        }
        if (const auto* request = std::get_if<ViewFrameDestination>(&selected))
        {
            auto* scene = scenes_.find(request->view.scene);
            auto* view = scene ? scene->views.find(request->view.view) : nullptr;
            if (!view) return cxx::unexpected(RenderError{kRuntimeIdentity});
            static const auto call = FrameDataCall::declare<CameraInput, &detail::CameraInputHandler::apply>();
            return BoundFrameData{&call, FrameViewTarget{view->camera, view->camera_revision}};
        }
        const auto& destination = std::get<FrameDataDestination>(selected);
        const auto* effect = effects_.find(destination.effect);
        if (!effect || destination.capability >= effect->implementations.size())
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        const auto& implementation = effect->implementations[destination.capability];
        if (destination.input >= implementation.frame_calls.size())
        {
            return cxx::unexpected(RenderError{kFrameContract});
        }
        const auto& input = implementation.frame_calls[destination.input];
        const auto& call = *input.method;
        if (call.target == EFrameDataTarget::STORAGE)
        {
            auto storage =
                resolveEffectResource(*effect, destination.capability, call.resource_dependency, destination.scope);
            if (!storage)
            {
                return cxx::unexpected(storage.error());
            }
            auto target = call.resolve_storage(*resources_, *storage);
            if (!target)
            {
                return cxx::unexpected(target.error());
            }
            return BoundFrameData{&call, VFrameDataTarget{*target}};
        }
        const auto* id = std::get_if<AttachmentId>(&destination.scope);
        auto* attachment = id ? attachments_.find(*id) : nullptr;
        if (!attachment || attachment->effect != destination.effect)
        {
            return cxx::unexpected(RenderError{kRuntimeIdentity});
        }
        if (call.target == EFrameDataTarget::PARAMETERS)
        {
            return BoundFrameData{&call, FrameParameterTarget{*attachment->instance, input.parameters}};
        }
        if (!uploads_) return cxx::unexpected(RenderError{kDeviceUnsupported});
        return BoundFrameData{
            &call,
            FrameDrawTarget{
                *attachment->instance,
                input.pass,
                *uploads_,
                this,
                [](void* context, TargetId id, std::uint32_t slot) noexcept -> RenderResult<FrameSampledImage>
                {
                    auto* target = static_cast<Impl*>(context)->targets_.find(id);
                    if (!target || !target->description.sampled || target->surface())
                    {
                        return cxx::unexpected(RenderError{kRuntimeIdentity});
                    }
                    const auto output = target->output().frames[slot];
                    return FrameSampledImage{output.image, output.sampled_index};
                }
            }
        };
    }

    void RenderServer::Impl::finishFrame(FrameRecord& frame, FrameCompletion result) noexcept
    {
        // A later input failure or an inactive output may bypass preparation entirely.
        // Settle every adopted attachment's source borrow before returning FrameStorage.
        if (frame.slot != UINT32_MAX)
        {
            const RenderResult<void> uploaded = result.error.type != 0 ? cxx::unexpected(result.error)
                : result.outcome == EFrameOutcome::NO_DRAW ? cxx::unexpected(RenderError{kCommandCancelled})
                : RenderResult<void>{};
            for (std::uint32_t i = 0; i < frame.dispatched; ++i)
            {
                const auto* destination = std::get_if<FrameDataDestination>(&frame.input.entries[i].destination);
                const auto* id = destination ? std::get_if<AttachmentId>(&destination->scope) : nullptr;
                if (id)
                {
                    attachments_.find(*id)->instance->finishDrawListUploads(frame.slot, uploaded);
                }
            }
        }
        auto* receiver = frame.receiver;
        const auto complete = frame.complete;
        frame.stage = FrameRecord::EStage::EMPTY;
        frame.input = {};
        frame.receiver = nullptr;
        frame.complete = nullptr;
        first_frame_ = (first_frame_ + 1) % frame_inputs_.size();
        --frame_count_;
        complete(receiver, result);
        // Terminal delivery owns its Response and can release its connected state immediately.
    }

    void RenderServer::Impl::activateFrame(FrameRecord& frame) noexcept
    {
        if (frame_resynchronization_ && !frame.input.resynchronize)
        {
            frame.error = RenderError{kFrameResynchronization};
        }
        if (device_->terminalError().type != 0)
        {
            frame.error = device_->terminalError();
            frame.outcome_unknown |= device_->terminalError().type == kOutcomeUnknown;
        }
        if (frame.error.type == 0)
        {
            if (!frames_->beginFrame(frame.slot))
            {
                return; // Keep the input queued until a physical FIF slice becomes reusable.
            }
            frame.stage = FrameRecord::EStage::APPLYING;
        }
    }

    void RenderServer::Impl::dispatchFramePhases(FrameRecord& frame) noexcept
    {
        const FrameDataView data{frame.input.blocks};
        while (frame.phase < frame.input.phases.size())
        {
            const auto phase = frame.input.phases[frame.phase++];
            frame.pending = phase.count; // Register the entire phase before any synchronous completion.
            for (std::uint32_t i = 0; i < phase.count; ++i)
            {
                const auto index = phase.first + i;
                const auto& entry = frame.input.entries[index];
                ++frame.dispatched;
                if (device_->terminalError().type != 0)
                {
                    completeFrameData(frame, index, device_->terminalError());
                    continue;
                }
                // Structural commands cannot overtake accepted frames, so admitted binding and block
                // identities remain valid. There is no per-frame Invocation cache or repeated type lookup.
                const auto binding = *resolveFrameData(entry.destination);
                const auto* root = frame.input.blocks[entry.payload.block].data() + entry.payload.offset;
                FrameDispatchContext context{
                    data, root, binding.target, frame, index, command_scope_,
                    server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery)
                };
                binding.call->invoke(binding.call->handler ? binding.call->handler->object.get() : nullptr, context);
            }
            if (frame.pending != 0 || frame.error.type != 0)
            {
                break;
            }
        }
        const bool all_inputs_complete =
            frame.pending == 0 && frame.error.type == 0 && frame.phase == frame.input.phases.size();
        if (all_inputs_complete)
        {
            frame.stage = FrameRecord::EStage::PREPARING_PROGRAMS;
        }
    }

    exec::task<RenderResult<void>> RenderServer::Impl::prepareRasterPrograms() noexcept
    {
        for (const auto& [instance, binding] : frame_features_)
        {
            const auto* raster = std::get_if<detail::FeatureBinding::RasterPreparation>(&binding->preparation);
            if (raster && raster->requires_programs(binding->value.get()))
            {
                auto prepared = co_await raster->prepare_programs(binding->value.get(), cpu_scheduler_);
                if (!prepared)
                {
                    co_return prepared;
                }
            }
        }
        co_return RenderResult<void>{};
    }

    void RenderServer::Impl::prepareFramePrograms(FrameRecord& frame) noexcept
    {
        if (active_frame_inputs_dirty_)
        {
            rebuildActiveFrameInputs();
        }
        if (auto applied = applyViewCameras(); !applied)
        {
            frame.error = applied.error();
            return;
        }
        const bool requires_programs = std::ranges::any_of(
            frame_features_,
            [](const auto& input) noexcept
            {
                const auto& binding = *input.second;
                const auto* raster = std::get_if<detail::FeatureBinding::RasterPreparation>(&binding.preparation);
                return raster && raster->requires_programs(binding.value.get());
            }
        );
        if (!requires_programs)
        {
            frame.stage = FrameRecord::EStage::ACQUIRING_OUTPUTS;
            return;
        }
        frame.pending = 1;
        const auto scheduler = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        auto prepared =
            prepareRasterPrograms() |
            stdexec::upon_error([](std::exception_ptr) noexcept -> RenderResult<void> { std::terminate(); }) |
            stdexec::upon_stopped(
                []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kCommandCancelled}); }
            ) |
            stdexec::continues_on(scheduler) |
            stdexec::upon_error([](cxx::execution::ScheduleError) noexcept -> RenderResult<void> { std::terminate(); }) |
            stdexec::then(
                [&frame](RenderResult<void> result) noexcept
                {
                    frame.pending = 0;
                    if (!result)
                    {
                        frame.error = result.error();
                    }
                    frame.stage = FrameRecord::EStage::ACQUIRING_OUTPUTS;
                }
            );
        // Accepted frames already prevent structural destruction. This uses their existing Scope;
        // no Feature owns a connected operation or an additional shutdown mechanism.
        stdexec::spawn(
            std::move(prepared),
            command_scope_.get_token(),
            stdexec::env{stdexec::prop{stdexec::get_scheduler, scheduler}}
        );
    }

    void RenderServer::Impl::failFrame(FrameRecord& frame) noexcept
    {
        const bool applied_inputs = frame.dispatched != 0;
        std::uint32_t accepted = 0;
        if (frame.slot != UINT32_MAX)
        {
            accepted = frames_->submittedBatches(frame.slot);
            if (!frames_->inputConsumed(frame.slot))
            {
                frames_->cancelSubmissions(frame.error);
            }
            frame_resynchronization_ |= applied_inputs;
        }
        // A queued frame can become stale after admission but before acquiring a physical slot.
        // Report the same pre-application rejection as admitFrame, so its caller can recollect.
        const bool is_stale_frame = frame.slot == UINT32_MAX && frame.error.type == kFrameResynchronization;
        const auto outcome = frame.outcome_unknown ? EFrameOutcome::UNKNOWN
            : is_stale_frame ? EFrameOutcome::REJECTED
            : accepted ? EFrameOutcome::PARTIALLY_SUBMITTED : EFrameOutcome::NOT_SUBMITTED;
        finishFrame(
            frame,
            FrameCompletion{
                frame.error,
                frame.input.sequence,
                outcome,
                accepted,
                frame_resynchronization_,
                frame.failed_entry
            }
        );
    }

    void RenderServer::Impl::submitFrameRendering(FrameRecord& frame) noexcept
    {
        frame.pending = 1;
        const auto delivery = server_context_.get_scheduler(cxx::execution::ScheduleMode::delivery);
        stdexec::spawn(
            stdexec::starts_on(delivery, submitCurrentFrame(frame.slot)) |
                stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                stdexec::upon_stopped(
                    []() noexcept -> RenderResult<void> { return cxx::unexpected(RenderError{kCommandCancelled}); }
                ) |
                stdexec::continues_on(delivery) |
                stdexec::upon_error([](auto) noexcept -> RenderResult<void> { std::terminate(); }) |
                stdexec::then(
                    [this, &frame](RenderResult<void> result) noexcept
                    {
                        frame.pending = 0;
                        finishFrameRendering(frame, std::move(result));
                    }
                ),
            command_scope_.get_token()
        );
    }

    void RenderServer::Impl::finishFrameRendering(FrameRecord& frame, RenderResult<void> submitted) noexcept
    {
        if (!submitted)
        {
            frame.error = submitted.error();
            frame.outcome_unknown |= frame.error.type == kOutcomeUnknown;
            // render() already retires a failed submission. Do not cancel the next physical slot.
            frame_resynchronization_ = true;
            const auto accepted = frames_->submittedBatches(frame.slot);
            finishFrame(
                frame,
                FrameCompletion{
                    frame.error,
                    frame.input.sequence,
                    frame.outcome_unknown
                        ? EFrameOutcome::UNKNOWN
                        : (accepted ? EFrameOutcome::PARTIALLY_SUBMITTED : EFrameOutcome::NOT_SUBMITTED),
                    accepted,
                    true
                }
            );
            return;
        }
        if (frame.error.type != 0)
        {
            failFrame(frame);
            return;
        }
        if (frames_->inputConsumed(frame.slot))
        {
            const bool no_draw = active_targets_.empty();
            frame_resynchronization_ = false;
            finishFrame(
                frame,
                FrameCompletion{
                    {},
                    frame.input.sequence,
                    no_draw ? EFrameOutcome::NO_DRAW : EFrameOutcome::SUBMITTED,
                    frames_->submittedBatches(frame.slot),
                    false
                }
            );
        }
    }

    void RenderServer::Impl::progressFrame() noexcept
    {
        if (frame_count_ == 0)
        {
            return;
        }
        auto& frame = frame_inputs_[first_frame_];
        if (stop_requested_.load() && frame.error.type == 0)
        {
            frame.error = RenderError{kCommandCancelled};
        }
        // Once output backpressure ended a frame for a structural request, do not apply
        // its stale queued successors before that request reaches the boundary.
        const bool reject_stale_input = deferred_request_.active() && frame_resynchronization_ &&
            frame.stage == FrameRecord::EStage::QUEUED && frame.error.type == 0;
        if (reject_stale_input)
        {
            frame.error = RenderError{kFrameResynchronization};
        }
        if (frame.error.type != 0 && frame.pending == 0)
        {
            failFrame(frame);
            return;
        }
        if (frame.stage == FrameRecord::EStage::QUEUED)
        {
            activateFrame(frame);
        }
        const bool can_dispatch =
            frame.stage == FrameRecord::EStage::APPLYING && frame.pending == 0 && frame.error.type == 0;
        if (can_dispatch)
        {
            dispatchFramePhases(frame);
        }
        if (frame.stage == FrameRecord::EStage::PREPARING_PROGRAMS && frame.pending == 0 && frame.error.type == 0)
        {
            prepareFramePrograms(frame);
        }
        if (frame.pending != 0)
        {
            return; // Upload and completion continuations progress at the service-loop safe points.
        }
        if (frame.error.type != 0)
        {
            failFrame(frame);
            return;
        }
        if (frame.stage == FrameRecord::EStage::ACQUIRING_OUTPUTS)
        {
            const auto acquired = acquireFrameOutputs(frame.slot);
            if (!acquired)
            {
                frame.error = acquired.error();
                failFrame(frame);
                return;
            }
            if (!*acquired)
            {
                if (deferred_request_.active())
                {
                    // Only an actually unavailable output may interrupt the older frame.
                    // Otherwise accepted frames finish normally before the structural request.
                    frame_resynchronization_ = true;
                    frame.error = RenderError{kDeviceBusy};
                    failFrame(frame);
                }
                return;
            }
            frame.stage = FrameRecord::EStage::SUBMITTING;
        }
        if (frame.stage == FrameRecord::EStage::SUBMITTING)
        {
            submitFrameRendering(frame);
        }
    }
} // namespace lux::render

namespace lux::render::runtime_detail
{
    void completeFrameData(FrameRecord& frame, std::uint32_t entry, RenderError error) noexcept
    {
        --frame.pending;
        if (error.type != 0)
        {
            frame.outcome_unknown |= error.type == kOutcomeUnknown;
            if (entry < frame.failed_entry)
            {
                frame.failed_entry = entry;
                frame.error = error;
            }
        }
    }
} // namespace lux::render::runtime_detail
