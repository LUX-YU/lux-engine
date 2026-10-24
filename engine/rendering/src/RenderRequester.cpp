#include <algorithm>
#include <deque>
#include <limits>
#include <lux/engine/rendering/RenderRequester.hpp>
#include <map>

namespace lux::rendering
{
    namespace ipc = platform::ipc;
    using namespace render;

    struct RenderRequester::Impl final
    {
        struct CorrelationTag;
        using Key = cxx::SlotKey<CorrelationTag>;

        struct PendingResponse final
        {
            Key key;
            ResponseNode* node = nullptr; // Borrowed from a connected operation; never deleted here.
            std::uint32_t frame = UINT32_MAX;
            std::uint32_t command = UINT32_MAX;
        };

        enum class EInputState : std::uint8_t
        {
            FREE,
            BUILDING,
            AWAITING_RESPONSE,
            COMPLETE
        };

        struct FrameCarrier final
        {
            explicit FrameCarrier(std::pmr::memory_resource& memory) noexcept : input(memory) {}

            FrameInput input;
            std::vector<FrameCommandGroup> groups;
            ResponseNode* terminal = nullptr;
            ipc::IpcMessage result;
            std::size_t remaining_responses = 0;
            EInputState state = EInputState::FREE;
            std::uint64_t sequence = 0;
            bool frame_response_received = false;
        };

        ipc::IpcRequester requester;
        std::optional<std::size_t> max_pending_requests;
        cxx::SlotMap<PendingResponse, CorrelationTag> pending;
        std::map<std::string, CommandInfo, std::less<>> commands;
        std::vector<FrameCommand> queued_frame_commands;
        // Cache normal frame payload blocks across carrier reuse; this is not an admission limit.
        // Larger payloads remain supported by the standard upstream allocator.
        std::pmr::unsynchronized_pool_resource frame_memory{{1, 1024 * 1024}};
        std::deque<FrameCarrier> frame_inputs;
        CommandId submit_frame;
        std::uint64_t next_frame = 1;
        FrameInput* building = nullptr;
        bool frame_input_requested = false;
        std::uint64_t resynchronization = 0;
        bool requires_resynchronization = false;
        enum class EConnectionState : std::uint8_t
        {
            OPEN,
            DRAINING,
            CLOSED
        };
        EConnectionState connection_state = EConnectionState::OPEN;

        Impl(ipc::IpcRequester value, RenderRequesterConfig config) noexcept
            : requester(std::move(value)), max_pending_requests(config.max_pending_requests)
        {
            if (max_pending_requests)
            {
                pending.reserve(*max_pending_requests);
            }
            if (config.max_frames_ahead)
            {
                for (std::size_t i = 0; i <= *config.max_frames_ahead; ++i)
                {
                    frame_inputs.emplace_back(frame_memory);
                }
            }
            for (std::uint32_t i = 0; i < frame_inputs.size(); ++i)
            {
                frame_inputs[i].input.slot_ = i;
            }
        }

        static RequestId correlation(Key key) noexcept
        {
            return {(std::uint64_t(key.gen) << 32) | (std::uint64_t(key.index) + 1)};
        }

        static Key key(RequestId id) noexcept
        {
            return {static_cast<std::uint32_t>(id.value) - 1, static_cast<std::uint32_t>(id.value >> 32)};
        }

        RequestId add(ResponseNode* node, std::uint32_t frame = UINT32_MAX, std::uint32_t command = UINT32_MAX) noexcept
        {
            const auto id = pending.emplace(PendingResponse{{}, node, frame, command});
            pending.find(id)->key = id;
            return correlation(id);
        }

        void completeFrame(FrameCarrier& frame) noexcept
        {
            if (!frame.frame_response_received || frame.remaining_responses != 0)
            {
                return;
            }
            auto* node = std::exchange(frame.terminal, nullptr);
            auto result = std::move(frame.result);
            frame.state = EInputState::COMPLETE;
            requires_resynchronization = result.get<FrameCompletion>().requires_resynchronization;
            if (requires_resynchronization)
            {
                ++resynchronization;
            }
            deliverResponse(*node, std::move(result), {});
            // The receiver can release/reuse this carrier. Do not read frame afterwards.
        }

        void rejectCommands(FrameCarrier& frame, RenderError error) noexcept
        {
            if (error.type == 0)
            {
                error = RenderError{kCommandContract};
            }
            for (auto& command : frame.input.commands)
            {
                const auto* found = pending.find(key(command.request_id));
                if (!found)
                {
                    continue;
                }
                auto* node = found->node;
                pending.erase(found->key);
                command.request_id = {};
                command.request.reset();
                if (node)
                {
                    deliverResponse(*node, {}, error);
                }
            }
            frame.remaining_responses = 0;
        }
    };

    void RenderRequester::deliverResponse(ResponseNode& node, ipc::IpcMessage&& message, RenderError error) noexcept
    {
        // The final receiver may destroy its stdexec state and task metadata. Callback code must
        // remain loaded until control returns to this protocol driver, including error delivery.
        auto code = std::move(node.code);
        node.deliver(node, std::move(message), error);
    }

    RenderRequester::RenderRequester(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    RenderRequester::~RenderRequester() noexcept
    {
        if (!impl_->pending.empty())
        {
            std::terminate(); // Input borrows and connected receivers require actual settlement, not abandonment.
        }
    }

    RenderRequester::CreateResult RenderRequester::create(
        ipc::IpcRequester requester,
        RenderRequesterConfig config
    ) noexcept
    {
        const bool invalid_capacity = config.max_pending_requests &&
                                      (*config.max_pending_requests == 0 || *config.max_pending_requests >= UINT32_MAX);
        const bool limited_main = config.max_frames_ahead && config.max_pending_requests;
        const bool invalid_lead = config.max_frames_ahead && *config.max_frames_ahead == UINT32_MAX;
        if (requester.id().isNull() || invalid_capacity || invalid_lead || limited_main)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        auto impl = std::make_unique<Impl>(std::move(requester), config);
        return std::unique_ptr<RenderRequester>(new RenderRequester(std::move(impl)));
    }

    ipc::RequesterId RenderRequester::id() const noexcept
    {
        return impl_->requester.id();
    }

    RenderResult<void> RenderRequester::setCommands(CommandList list) noexcept
    {
        if (list.error.type != 0)
        {
            return cxx::unexpected(list.error);
        }
        std::map<std::string, CommandInfo, std::less<>> candidate;
        for (auto& entry : list.commands)
        {
            const auto name = entry.name;
            if (entry.id.isNull() || !candidate.emplace(name, std::move(entry)).second)
            {
                return cxx::unexpected(RenderError{kCommandContract});
            }
        }
        const auto submit = candidate.find(SubmitFrame::name);
        if (!impl_->frame_inputs.empty())
        {
            if (submit == candidate.end())
            {
                return cxx::unexpected(RenderError{kRuntimeUnsupported});
            }
            if (auto valid = compatibleCommands(submit->second.descriptor(), commandDescriptor<SubmitFrame>()); !valid)
            {
                return valid;
            }
        }
        impl_->submit_frame = submit == candidate.end() ? CommandId{} : submit->second.id;
        impl_->commands = std::move(candidate);
        return {};
    }

    RenderResult<CommandId> RenderRequester::findCommand(const CommandDescriptor& descriptor) const noexcept
    {
        const auto found = impl_->commands.find(descriptor.name.name());
        if (found == impl_->commands.end() || !found->second.bound)
        {
            return cxx::unexpected(RenderError{kRuntimeUnsupported});
        }
        if (auto valid = compatibleCommands(found->second.descriptor(), descriptor); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return found->second.id;
    }

    RenderResult<RequestId> RenderRequester::admit(ResponseNode& node, ipc::RequesterId source) noexcept
    {
        if (impl_->connection_state != Impl::EConnectionState::OPEN || source != id())
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (impl_->max_pending_requests && impl_->pending.size() >= *impl_->max_pending_requests)
        {
            return cxx::unexpected(RenderError{kRuntimeCapacity});
        }
        return impl_->add(&node);
    }

    RenderResult<void> RenderRequester::sendMessage(
        CommandId command,
        RequestId request,
        ipc::IpcMessage& message
    ) noexcept
    {
        auto result = impl_->requester.trySend(commandTag(command), request.value, message);
        if (!result)
        {
            if (result.error() == ipc::EIpcError::CLOSED)
            {
                impl_->connection_state = Impl::EConnectionState::CLOSED;
            }
            return cxx::unexpected(
                RenderError{result.error() == ipc::EIpcError::WOULD_BLOCK ? kRuntimeCapacity : kRuntimeState}
            );
        }
        return {};
    }

    void RenderRequester::revoke(RequestId id) noexcept
    {
        impl_->pending.erase(Impl::key(id));
    }

    RenderResult<void> RenderRequester::queueFrameCommand(
        CommandId command,
        RequestId request,
        ipc::IpcMessage& message
    ) noexcept
    {
        if (impl_->frame_inputs.empty())
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        impl_->queued_frame_commands.push_back({command, request, std::move(message)});
        return {};
    }

    RenderResult<FrameInput*> RenderRequester::beginFrame() noexcept
    {
        if (impl_->connection_state != Impl::EConnectionState::OPEN || impl_->building)
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        for (auto& frame : impl_->frame_inputs)
        {
            if (frame.state == Impl::EInputState::FREE)
            {
                frame.state = Impl::EInputState::BUILDING;
                impl_->building = &frame.input;
                impl_->frame_input_requested = false;
                frame.input.resynchronize = impl_->requires_resynchronization;
                frame.input.commands.swap(impl_->queued_frame_commands);
                for (std::uint32_t i = 0; i < frame.input.commands.size(); ++i)
                {
                    auto& pending = *impl_->pending.find(Impl::key(frame.input.commands[i].request_id));
                    pending.frame = frame.input.slot_;
                    pending.command = i;
                }
                if (!frame.input.commands.empty())
                {
                    // Arbitrary queued control commands may depend on their predecessor. Explicit
                    // parallel groups remain available for the application's ordinary frame inputs.
                    for (std::size_t i = 1; i <= frame.input.commands.size(); ++i)
                    {
                        frame.input.group_ends.push_back(i);
                    }
                }
                return &frame.input;
            }
        }
        return cxx::unexpected(RenderError{kRuntimeCapacity});
    }

    FrameInput* RenderRequester::buildingFrame() noexcept
    {
        return impl_->building;
    }

    bool RenderRequester::hasFrameCapacity() const noexcept
    {
        return impl_->connection_state == Impl::EConnectionState::OPEN && !impl_->building &&
               std::ranges::any_of(
                   impl_->frame_inputs,
                   [](const auto& frame) noexcept { return frame.state == Impl::EInputState::FREE; }
               );
    }

    std::uint64_t RenderRequester::resynchronization() const noexcept
    {
        return impl_->resynchronization;
    }

    bool RenderRequester::isBuildingInput(const FrameInput& input, ipc::RequesterId source) const noexcept
    {
        const bool valid_carrier = input.slot_ < impl_->frame_inputs.size() &&
                                   &impl_->frame_inputs[input.slot_].input == &input &&
                                   impl_->frame_inputs[input.slot_].state == Impl::EInputState::BUILDING;
        return valid_carrier && source == id() && impl_->connection_state == Impl::EConnectionState::OPEN;
    }

    RenderResult<void> RenderRequester::releaseFrame(FrameInput& input) noexcept
    {
        if (input.slot_ >= impl_->frame_inputs.size() || &impl_->frame_inputs[input.slot_].input != &input)
        {
            return cxx::unexpected(RenderError{kCommandContract});
        }
        auto& frame = impl_->frame_inputs[input.slot_];
        if (frame.state != Impl::EInputState::BUILDING && frame.state != Impl::EInputState::COMPLETE)
        {
            return cxx::unexpected(RenderError{kRuntimeState});
        }
        if (frame.state == Impl::EInputState::BUILDING)
        {
            impl_->building = nullptr;
            impl_->rejectCommands(frame, RenderError{kCommandCancelled});
        }
        input.commands.clear();
        input.group_ends.clear();
        input.responses.clear();
        input.payloads_.release();
        input.resynchronize = false;
        frame.groups.clear();
        frame.result.reset();
        frame.state = Impl::EInputState::FREE;
        return {};
    }

    void RenderRequester::sendFrame(FrameInput& input, ResponseNode& terminal) noexcept
    {
        const auto reject = [&](RenderError error) noexcept { deliverResponse(terminal, {}, error); };
        if (!isBuildingInput(input, id()))
        {
            reject(RenderError{kRuntimeState});
            return;
        }
        if (impl_->submit_frame.isNull())
        {
            reject(RenderError{kRuntimeUnsupported});
            return;
        }
        // UINT32_MAX identifies the frame terminal in a correlation. Keep that representation boundary;
        // it is not a configurable request budget and is checked before publishing any child correlation.
        if (input.commands.size() >= UINT32_MAX)
        {
            reject(RenderError{kCommandContract});
            return;
        }
        auto& frame = impl_->frame_inputs[input.slot_];
        frame.groups.clear();
        std::size_t first = 0;
        for (auto end : input.group_ends)
        {
            if (end < first || end > input.commands.size())
            {
                reject(RenderError{kCommandContract});
                return;
            }
            frame.groups.push_back({std::span(input.commands).subspan(first, end - first)});
            first = end;
        }
        if (first != input.commands.size())
        {
            reject(RenderError{kCommandContract});
            return;
        }
        input.responses.resize(input.commands.size());
        for (std::uint32_t i = 0; i < input.commands.size(); ++i)
        {
            if (input.commands[i].request_id.value == 0)
            {
                input.commands[i].request_id = impl_->add(nullptr, input.slot_, i);
            }
        }
        const auto root = impl_->add(&terminal, input.slot_);
        frame.sequence = impl_->next_frame;
        ipc::IpcMessage message;
        message.emplace<FrameSubmission>(FrameSubmission{frame.sequence, frame.groups, input.resynchronize});
        if (auto sent = sendMessage(impl_->submit_frame, root, message); !sent)
        {
            for (const auto& command : input.commands)
            {
                const auto* pending = impl_->pending.find(Impl::key(command.request_id));
                if (pending && !pending->node)
                {
                    revoke(command.request_id);
                }
            }
            for (auto& command : input.commands)
            {
                if (!impl_->pending.find(Impl::key(command.request_id)))
                {
                    command.request_id = {};
                }
            }
            revoke(root);
            reject(sent.error());
            return;
        }
        ++impl_->next_frame;
        frame.remaining_responses = input.commands.size();
        frame.frame_response_received = false;
        frame.terminal = &terminal;
        frame.state = Impl::EInputState::AWAITING_RESPONSE;
        impl_->building = nullptr;
    }

    std::size_t RenderRequester::consumeResponses(std::size_t limit) noexcept
    {
        std::size_t received = 0;
        for (; received < limit; ++received)
        {
            std::uint64_t correlation = 0;
            ipc::IpcMessage message;
            const auto status = impl_->requester.tryReceive(correlation, message);
            if (status != ipc::EIpcReceive::RECEIVED)
            {
                if (status == ipc::EIpcReceive::CLOSED)
                {
                    impl_->connection_state = Impl::EConnectionState::CLOSED;
                }
                break;
            }
            const auto key = Impl::key({correlation});
            const auto* found = impl_->pending.find(key);
            if (!found)
            {
                continue; // A stale correlation can never address a reused generation.
            }
            const auto entry = *found;
            const bool invalid_frame_response = entry.frame != UINT32_MAX && entry.command == UINT32_MAX &&
                                                message.typeHash() != cxx::typeToken<FrameCompletion>().hash();
            if (invalid_frame_response)
            {
                // A malformed reply cannot prove that the Server released its frame borrow.
                // Retain this correlation until the application supplies the shutdown boundary.
                impl_->connection_state = Impl::EConnectionState::CLOSED;
                ++received;
                break;
            }
            impl_->pending.erase(key);
            if (entry.frame == UINT32_MAX)
            {
                deliverResponse(*entry.node, std::move(message), {});
                continue;
            }
            auto& frame = impl_->frame_inputs[entry.frame];
            if (entry.command != UINT32_MAX)
            {
                --frame.remaining_responses;
                if (entry.node)
                {
                    deliverResponse(*entry.node, std::move(message), {});
                }
                else
                {
                    frame.input.responses[entry.command] = std::move(message);
                }
            }
            else
            {
                if (message.get<FrameCompletion>().outcome == EFrameOutcome::REJECTED)
                {
                    // Rejected admission never dispatched children; their original Requests remain here.
                    impl_->rejectCommands(frame, message.get<FrameCompletion>().error);
                }
                frame.result = std::move(message);
                frame.frame_response_received = true;
            }
            impl_->completeFrame(frame);
        }
        return received;
    }

    bool RenderRequester::closed() const noexcept
    {
        return impl_->connection_state == Impl::EConnectionState::CLOSED;
    }

    std::size_t RenderRequester::pendingRequests() const noexcept
    {
        return impl_->pending.size();
    }

    bool RenderRequester::hasQueuedFrameCommands() const noexcept
    {
        return !impl_->queued_frame_commands.empty();
    }

    void RenderRequester::requestFrameInput() noexcept
    {
        impl_->frame_input_requested = true;
    }

    bool RenderRequester::needsFrameInput() const noexcept
    {
        return impl_->frame_input_requested || hasQueuedFrameCommands();
    }

    void RenderRequester::beginClose() noexcept
    {
        impl_->frame_input_requested = false;
        if (impl_->connection_state == Impl::EConnectionState::OPEN)
        {
            impl_->connection_state = Impl::EConnectionState::DRAINING;
        }
        if (impl_->building)
        {
            static_cast<void>(releaseFrame(*impl_->building));
        }
        auto unsent = std::move(impl_->queued_frame_commands);
        for (auto& command : unsent)
        {
            const auto entry = *impl_->pending.find(Impl::key(command.request_id));
            impl_->pending.erase(entry.key);
            command.request.reset();
            deliverResponse(*entry.node, {}, RenderError{kCommandCancelled});
        }
    }

    bool RenderRequester::close() noexcept
    {
        if (!impl_->pending.empty())
        {
            return false;
        }
        impl_->requester.reset();
        impl_->connection_state = Impl::EConnectionState::CLOSED;
        return true;
    }

    void RenderRequester::settleClosed() noexcept
    {
        consumeResponses(SIZE_MAX);
        beginClose();
        impl_->connection_state = Impl::EConnectionState::CLOSED;
        impl_->requester.reset();
        while (!impl_->pending.empty())
        {
            const auto entry = *impl_->pending.begin();
            impl_->pending.erase(entry.key);
            if (entry.frame == UINT32_MAX)
            {
                deliverResponse(*entry.node, {}, RenderError{kOutcomeUnknown});
                continue;
            }
            auto& frame = impl_->frame_inputs[entry.frame];
            if (entry.command != UINT32_MAX)
            {
                if (frame.state == Impl::EInputState::AWAITING_RESPONSE)
                {
                    --frame.remaining_responses;
                }
                if (entry.node)
                {
                    const bool submitted = frame.state == Impl::EInputState::AWAITING_RESPONSE;
                    frame.input.commands[entry.command].request.reset();
                    deliverResponse(*entry.node, {}, RenderError{submitted ? kOutcomeUnknown : kCommandCancelled});
                }
            }
            else
            {
                auto& result = frame.result.emplace<FrameCompletion>();
                result.error = RenderError{kOutcomeUnknown};
                result.sequence = frame.sequence;
                result.outcome = EFrameOutcome::UNKNOWN;
                result.requires_resynchronization = true;
                frame.frame_response_received = true;
            }
            if (frame.state == Impl::EInputState::AWAITING_RESPONSE)
            {
                impl_->completeFrame(frame);
            }
        }
    }
} // namespace lux::rendering
