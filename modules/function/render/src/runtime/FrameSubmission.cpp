#include <exec/create.hpp>
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
        FrameSubmission input,
        void* receiver,
        void (*complete)(void*, FrameCompletion) noexcept
    ) noexcept
    {
        const auto reject = [&](RenderError error) noexcept
        { complete(receiver, {error, input.sequence, EFrameOutcome::REJECTED, 0, frame_resynchronization_}); };
        if (stop_requested.load())
        {
            reject(RenderError{kCommandCancelled});
            return;
        }
        if (input.sequence == 0 || input.sequence <= last_frame_sequence_)
        {
            reject(RenderError{kCommandContract});
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
        frame.invocations.clear();
        std::size_t count = 0;
        for (const auto& group : input.groups)
        {
            if (group.commands.size() > std::numeric_limits<std::size_t>::max() - count)
            {
                reject(RenderError{kRuntimeCapacity});
                return;
            }
            count += group.commands.size();
        }
        frame.invocations.reserve(count);
        for (const auto& group : input.groups)
        {
            for (auto& command : group.commands)
            {
                const auto* entry = command_registry_.findCommand(command.command);
                const bool is_control = command.command == submit_frame_command_ || command.command == stop_command_ ||
                                        command.command == select_binding_command_;
                const bool is_invalid = !entry || command.request_id.isNull() || is_control;
                if (is_invalid)
                {
                    frame.invocations.clear();
                    reject(RenderError{kCommandContract});
                    return;
                }
                const bool is_type_mismatch = command.request.typeHash() != entry->description->request.hash();
                if (is_type_mismatch || !entry->selectedBinding())
                {
                    frame.invocations.clear();
                    reject(RenderError{is_type_mismatch ? kCommandContract : kRuntimeUnsupported});
                    return;
                }
                frame.invocations.push_back({&command, {}, entry->make_error});
            }
        }
        frame.input = input;
        frame.stage = FrameRecord::EStage::QUEUED;
        frame.group = frame.dispatched = frame.group_pending = 0;
        frame.slot = UINT32_MAX;
        frame.error = {};
        frame.receiver = receiver;
        frame.complete = complete;
        last_frame_sequence_ = input.sequence;
        ++frame_count_;
    }

    void RenderServer::Impl::skipFrameCommands(FrameRecord& frame, RenderError error) noexcept
    {
        for (; frame.dispatched < frame.invocations.size(); ++frame.dispatched)
        {
            auto& invocation = frame.invocations[frame.dispatched];
            const ResponseRoute route{frame_source_, invocation.input->request_id};
            invocation.make_error(response_outbox_.prepare(route), error);
            invocation.input->request.reset();
        }
    }

    void RenderServer::Impl::finishFrame(FrameRecord& frame, FrameCompletion result) noexcept
    {
        auto* receiver = frame.receiver;
        const auto complete = frame.complete;
        frame.stage = FrameRecord::EStage::EMPTY;
        frame.input = {};
        frame.invocations.clear();
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
        if (frame.error.type == 0)
        {
            if (!frames->beginFrame(frame.slot))
            {
                return; // Keep the input queued until a physical FIF slice becomes reusable.
            }
            // Directory entries are not erased while running. Resolve the selected method at
            // activation, so a frame-boundary selection affects queued, not already started frames.
            for (auto& invocation : frame.invocations)
            {
                const auto* selected = command_registry_.findCommand(invocation.input->command)->selectedBinding();
                if (!selected)
                {
                    frame.error = RenderError{kRuntimeUnsupported};
                    break;
                }
                invocation.handler = selected->call;
            }
            frame.stage = FrameRecord::EStage::APPLYING;
        }
    }

    void RenderServer::Impl::dispatchFrameGroups(FrameRecord& frame) noexcept
    {
        while (frame.group < frame.input.groups.size())
        {
            const auto count = frame.input.groups[frame.group++].commands.size();
            frame.group_pending = count; // Register the whole group before synchronous completion can reenter.
            for (std::size_t i = 0; i < count; ++i)
            {
                auto& invocation = frame.invocations[frame.dispatched++];
                auto& command = *invocation.input;
                CommandDispatchContext context{
                    command.request,
                    {frame_source_, command.request_id},
                    response_outbox_,
                    command_scope_,
                    render_loop_,
                    &frame
                };
                invocation.handler.invoke(invocation.handler.object, context);
                command.request.reset();
                ++commands;
            }
            if (frame.group_pending != 0 || frame.error.type != 0)
            {
                break;
            }
        }
        const bool groups_complete =
            frame.group_pending == 0 && frame.error.type == 0 && frame.group == frame.input.groups.size();
        if (groups_complete)
        {
            frame.stage = FrameRecord::EStage::SUBMITTING;
        }
    }

    void RenderServer::Impl::failFrame(FrameRecord& frame) noexcept
    {
        const bool applied_commands = frame.dispatched != 0;
        skipFrameCommands(frame, frame.error);
        std::uint32_t accepted = 0;
        if (frame.slot != UINT32_MAX)
        {
            accepted = frames->submittedBatches(frame.slot);
            if (!frames->inputConsumed(frame.slot))
            {
                frames->cancelSubmissions(frame.error);
            }
            frame_resynchronization_ |= applied_commands;
        }
        finishFrame(
            frame,
            FrameCompletion{
                frame.error,
                frame.input.sequence,
                accepted ? EFrameOutcome::PARTIALLY_SUBMITTED : EFrameOutcome::NOT_SUBMITTED,
                accepted,
                frame_resynchronization_
            }
        );
    }

    void RenderServer::Impl::submitFrameRendering(FrameRecord& frame) noexcept
    {
        const auto submitted = submitCurrentFrame();
        if (!submitted)
        {
            frame.error = submitted.error();
            // render() already retires a failed submission. Do not cancel the next physical slot.
            frame_resynchronization_ = true;
            const auto accepted = frames->submittedBatches(frame.slot);
            finishFrame(
                frame,
                FrameCompletion{
                    frame.error,
                    frame.input.sequence,
                    accepted ? EFrameOutcome::PARTIALLY_SUBMITTED : EFrameOutcome::NOT_SUBMITTED,
                    accepted,
                    true
                }
            );
            return;
        }
        if (frames->inputConsumed(frame.slot))
        {
            const bool no_draw = active_targets.empty();
            frame_resynchronization_ = false;
            finishFrame(
                frame,
                FrameCompletion{
                    {},
                    frame.input.sequence,
                    no_draw ? EFrameOutcome::NO_DRAW : EFrameOutcome::SUBMITTED,
                    frames->submittedBatches(frame.slot),
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
        if (stop_requested.load() && frame.error.type == 0)
        {
            frame.error = RenderError{kCommandCancelled};
        }
        if (frame.stage == FrameRecord::EStage::QUEUED)
        {
            activateFrame(frame);
        }
        const bool can_dispatch =
            frame.stage == FrameRecord::EStage::APPLYING && frame.group_pending == 0 && frame.error.type == 0;
        if (can_dispatch)
        {
            dispatchFrameGroups(frame);
        }
        if (frame.group_pending != 0)
        {
            return; // Upload and completion continuations progress at the service-loop safe points.
        }
        if (frame.error.type != 0)
        {
            failFrame(frame);
            return;
        }
        if (frame.stage == FrameRecord::EStage::SUBMITTING)
        {
            submitFrameRendering(frame);
        }
    }
} // namespace lux::render
