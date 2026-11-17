#include <exec/create.hpp>
#include <lux/engine/render/resource/ui/UiInputHandler.hpp>
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

    auto RenderServer::Impl::submitUiFrame(SubmitUiFrame::Request&& input) noexcept
    {
        return exec::create<stdexec::set_value_t(FrameCompletion)>([this, input](auto& context) noexcept
        {
            using Context = std::remove_reference_t<decltype(context)>;
            admitFrame(input.frame, &context, [](void* value, FrameCompletion result) noexcept
            {
                auto& context = *static_cast<Context*>(value);
                stdexec::set_value(std::move(context.receiver), std::move(result));
            }, input.draws);
        });
    }

    void RenderServer::Impl::bindSubmitFrameCommand() noexcept
    {
        bindFrameworkCommand<SubmitFrame, &Impl::submitFrame>();
        bindFrameworkCommand<SubmitUiFrame, &Impl::submitUiFrame>();
    }

    void
    RenderServer::Impl::queryFrameInputLayout(const AttachmentId& id, QueryFrameInputLayout::Response& output) noexcept
    {
        const auto* attachment = attachments_.find(id);
        if (!attachment) { output.error = {kRuntimeIdentity}; return; }
        output = {{}, attachment->input_layout};
    }

    void RenderServer::Impl::updateViewCamera(const UpdateViewCamera::Request& input, AckResponse& output) noexcept
    {
        auto* scene = scenes_.find(input.view.scene);
        auto* view = scene ? scene->views.find(input.view.view) : nullptr;
        if (!view) { output.error = {kRuntimeIdentity}; return; }
        const auto applied = detail::CameraInputHandler::apply(view->camera, view->camera_revision, input.camera);
        output.error = applied ? RenderError{} : applied.error();
    }

    void RenderServer::Impl::admitFrame(
        RenderFrame input,
        void* receiver,
        void (*complete)(void*, FrameCompletion) noexcept,
        std::span<const UiDrawInput> ui
    ) noexcept
    {
        const auto reject = [&](RenderError error, std::uint32_t entry = UINT32_MAX) noexcept
        { complete(receiver, {error, EFrameOutcome::REJECTED, 0, frame_resynchronization_, entry}); };
        if (stop_requested_.load()) { reject(RenderError{kCommandCancelled}); return; }
        if (const auto error = device_->terminalError(); error.type != 0) { reject(error); return; }
        if (frame_count_ == frame_inputs_.size()) { reject(RenderError{kRuntimeCapacity}); return; }
        if (frame_resynchronization_ && !input.resynchronize) { reject(RenderError{kFrameResynchronization}); return; }
        if (input.data)
        {
            const auto* attachment = attachments_.find(input.attachment);
            const bool is_missing_target = attachment == nullptr;
            const bool is_invalid_owner = !input.data->sealed();
            const bool is_layout_mismatch = !is_missing_target && !is_invalid_owner &&
                !input.data->layout().hasSameIdentity(attachment->input_layout);
            const bool is_invalid_packet = is_missing_target || is_invalid_owner || is_layout_mismatch;
            if (is_invalid_packet) { reject(RenderError{kFrameContract}); return; }
            for (const auto& bound : attachment->inputs)
            {
                // Entry generations remain mutable facts even though declaration/ABI/indices are cold.
                if (resources_->resourceInstance(bound.identity) != bound.object)
                { reject(RenderError{kStorageIdentity}); return; }
            }
        }
        for (const auto& draw : ui)
        {
            const auto* attachment = attachments_.find(draw.attachment);
            const auto* effect = attachment ? effects_.find(attachment->effect) : nullptr;
            const bool has_draw = effect && draw.draw < effect->draw_inputs.size();
            const bool is_invalid_draw = !has_draw || !uploads_;
            if (is_invalid_draw) { reject(RenderError{kFrameContract}); return; }
        }
        auto& frame = frame_inputs_[(first_frame_ + frame_count_) % frame_inputs_.size()];
        frame.input = input;
        frame.ui_inputs = ui;
        frame.stage = FrameRecord::EStage::QUEUED;
        frame.dispatched = frame.pending = 0;
        frame.failed_entry = UINT32_MAX;
        frame.outcome_unknown = false;
        frame.slot = UINT32_MAX;
        frame.error = {};
        frame.receiver = receiver;
        frame.complete = complete;
        ++frame_count_;
    }

    void RenderServer::Impl::finishFrame(FrameRecord& frame, FrameCompletion result) noexcept
    {
        // A later input failure or an inactive output may bypass preparation entirely.
        // Settle every adopted attachment's source borrow before returning the column/UI source borrow.
        if (frame.slot != UINT32_MAX)
        {
            const RenderResult<void> uploaded = result.error.type != 0 ? cxx::unexpected(result.error)
                : result.outcome == EFrameOutcome::NO_DRAW ? cxx::unexpected(RenderError{kCommandCancelled})
                : RenderResult<void>{};
            for (const auto& input : frame.ui_inputs)
            {
                attachments_.find(input.attachment)->instance->finishDrawListUploads(frame.slot, uploaded);
            }
        }

        auto* receiver = frame.receiver;
        const auto complete = frame.complete;
        frame.stage = FrameRecord::EStage::EMPTY;
        frame.input = {};
        frame.ui_inputs = {};
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

    void RenderServer::Impl::applyFrameInputs(FrameRecord& frame) noexcept
    {
        if (frame.input.data)
        {
            const auto& packet = *frame.input.data;
            const auto& inputs = attachments_.find(frame.input.attachment)->inputs;
            for (std::uint32_t i = 0; i < inputs.size(); ++i)
            {
                const auto& input = inputs[i];
                const bool has_updates = std::ranges::any_of(input.columns, [&](auto column) noexcept
                { return packet.column(column).count != 0; });
                if (!has_updates) continue;
                ++frame.dispatched;
                auto applied = input.call->invoke(input.object, packet, input.columns);
                if (!applied)
                {
                    frame.error = applied.error();
                    frame.outcome_unknown |= frame.error.type == kOutcomeUnknown;
                    frame.failed_entry = i;
                    return;
                }
            }
        }
        for (const auto& input : frame.ui_inputs)
        {
            auto& attachment = *attachments_.find(input.attachment);
            const auto pass = effects_.find(attachment.effect)->draw_inputs[input.draw];
            FrameDrawTarget target{
                *attachment.instance, pass, *uploads_, this,
                [](void* context, TargetId id, std::uint32_t slot) noexcept -> RenderResult<FrameSampledImage>
                {
                    auto* target = static_cast<Impl*>(context)->targets_.find(id);
                    if (!target || !target->description.sampled || target->surface())
                        return cxx::unexpected(RenderError{kRuntimeIdentity});
                    const auto output = target->output().frames[slot];
                    return FrameSampledImage{output.image, output.sampled_index};
                }
            };
            ++frame.dispatched;
            auto applied = detail::UiInputHandler::draw(*resources_, target, input.data);
            if (!applied)
            {
                frame.error = applied.error();
                frame.outcome_unknown |= frame.error.type == kOutcomeUnknown;
                return;
            }
        }
        frame.stage = FrameRecord::EStage::PREPARING_PROGRAMS;
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
            applyFrameInputs(frame);
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
