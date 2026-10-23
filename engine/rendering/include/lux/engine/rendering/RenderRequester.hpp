#pragma once

#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <memory>
#include <optional>
#include <stdexec/execution.hpp>
#include <vector>

namespace lux::rendering
{
    class RenderRequester;

    template <render::RenderCommand C> class BoundCommand final
    {
    private:
        friend class RenderRequester;

        BoundCommand(render::CommandId command, platform::ipc::RequesterId source) noexcept
            : command_(command), source_(source)
        {
        }

        render::CommandId command_;
        platform::ipc::RequesterId source_;
    };

    struct RenderRequesterConfig final
    {
        std::size_t request_capacity = 1024;
        // Absent for an independent Asset Client. The designated Game Client uses L + 1 carriers.
        std::optional<std::uint32_t> max_frames_ahead;
    };

    // Client-owned message carriers. Source spans in these Requests remain the application's
    // borrowed frame data; retain them until completion, or until settleClosed() after host shutdown.
    struct FrameInput final
    {
        std::vector<render::FrameCommand> commands;
        // Exclusive command end indices, increasing; the last end equals commands.size().
        std::vector<std::size_t> group_ends;
        std::vector<platform::ipc::IpcMessage> responses;
        bool resynchronize = false;

        [[nodiscard]] std::uint32_t carrierIndex() const noexcept
        {
            return slot_;
        }

    private:
        friend class RenderRequester;
        std::uint32_t slot_ = 0;
    };

    // One application-thread SPSC Link. Owns finite input carriers and correlation only; stdexec
    // owns connected operations. consumeResponses() and start() run on the same application thread.
    class RenderRequester final
    {
        struct ResponseNode
        {
            using Deliver = void (*)(ResponseNode&, platform::ipc::IpcMessage&&, render::RenderError) noexcept;

            ResponseNode(Deliver deliver) noexcept : deliver(deliver)
            {
            }

            ResponseNode(const ResponseNode&) = delete;
            ResponseNode& operator=(const ResponseNode&) = delete;
            Deliver deliver;
        };

    public:
        using CreateResult = render::RenderResult<std::unique_ptr<RenderRequester>>;

        [[nodiscard]] static CreateResult create(platform::ipc::IpcRequester, RenderRequesterConfig) noexcept;
        ~RenderRequester() noexcept;
        RenderRequester(const RenderRequester&) = delete;
        RenderRequester& operator=(const RenderRequester&) = delete;

        [[nodiscard]] platform::ipc::RequesterId id() const noexcept;

        // Cold catalog adoption, normally from ListCommands obtained through the host bootstrap.
        // Existing bound IDs retain their source qualification; refresh does not mutate a sent Request.
        [[nodiscard]] render::RenderResult<void> setCommands(render::CommandList) noexcept;

        template <render::RenderCommand C> [[nodiscard]] render::RenderResult<BoundCommand<C>> bind() const noexcept
        {
            auto command = findCommand(render::commandDescriptor<C>());
            if (!command)
            {
                return cxx::unexpected(command.error());
            }
            return BoundCommand<C>{*command, id()};
        }

        template <render::RenderCommand C> class RequestSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(typename C::Response)>;

            template <class Receiver> struct Operation final : ResponseNode
            {
                using operation_state_concept = stdexec::operation_state_t;
                RenderRequester& requester;
                BoundCommand<C> command;
                typename C::Request& input;
                Receiver receiver;

                Operation(
                    RenderRequester& requester,
                    BoundCommand<C> command,
                    typename C::Request& input,
                    Receiver receiver
                ) noexcept
                    : ResponseNode{&finish<Operation, C>},
                      requester(requester),
                      command(command),
                      input(input),
                      receiver(std::move(receiver))
                {
                }

                void start() & noexcept
                {
                    if (stdexec::get_stop_token(stdexec::get_env(receiver)).stop_requested())
                    {
                        finish<Operation, C>(*this, {}, render::RenderError{render::kCommandCancelled});
                        return;
                    }
                    requester.send<C>(command, input, *this);
                }
            };

            template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{requester_, command_, input_, std::move(receiver)};
            }

        private:
            friend class RenderRequester;

            RequestSender(RenderRequester& requester, BoundCommand<C> command, typename C::Request& input) noexcept
                : requester_(requester), command_(command), input_(input)
            {
            }

            RenderRequester& requester_;
            BoundCommand<C> command_;
            typename C::Request& input_;
        };

        // Lazy. Failed admission/send leaves input intact. After send, cancellation cannot prove
        // that the Server stopped reading a borrowed payload: the actual Response still settles it.
        template <render::RenderCommand C>
            requires(!C::frame_scoped && !std::same_as<C, render::SubmitFrame>)
        [[nodiscard]] RequestSender<C> request(BoundCommand<C> command, typename C::Request& input) noexcept
        {
            return {*this, command, input};
        }

        class FrameSender final
        {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(render::FrameCompletion)>;

            template <class Receiver> struct Operation final : ResponseNode
            {
                using operation_state_concept = stdexec::operation_state_t;
                RenderRequester& requester;
                FrameInput& input;
                Receiver receiver;

                Operation(RenderRequester& requester, FrameInput& input, Receiver receiver) noexcept
                    : ResponseNode{&finish<Operation, render::SubmitFrame>},
                      requester(requester),
                      input(input),
                      receiver(std::move(receiver))
                {
                }

                void start() & noexcept
                {
                    if (stdexec::get_stop_token(stdexec::get_env(receiver)).stop_requested())
                    {
                        finish<Operation, render::SubmitFrame>(
                            *this,
                            {},
                            render::RenderError{render::kCommandCancelled}
                        );
                        return;
                    }
                    requester.sendFrame(input, *this);
                }
            };

            template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{requester_, input_, std::move(receiver)};
            }

        private:
            friend class RenderRequester;

            FrameSender(RenderRequester& requester, FrameInput& input) noexcept : requester_(requester), input_(input)
            {
            }

            RenderRequester& requester_;
            FrameInput& input_;
        };

        // Acquire before preparing game-frame data. Capacity exhaustion means the game thread
        // consumes/waits at its normal safe point; it must not prepare another unbounded frame.
        [[nodiscard]] render::RenderResult<FrameInput*> beginFrame() noexcept;

        template <render::RenderCommand C>
        [[nodiscard]] render::RenderResult<void> append(
            FrameInput& frame,
            BoundCommand<C> command,
            typename C::Request input
        ) noexcept
        {
            if (!isBuildingInput(frame, command.source_))
            {
                return cxx::unexpected(render::RenderError{render::kRuntimeState});
            }
            auto& entry = frame.commands.emplace_back();
            entry.command = command.command_;
            entry.request.emplace<typename C::Request>(std::move(input));
            return {};
        }

        [[nodiscard]] FrameSender submitFrame(FrameInput& input) noexcept
        {
            return {*this, input};
        }

        // Retains vector capacities. Only an unsubmitted or completed carrier can be released.
        [[nodiscard]] render::RenderResult<void> releaseFrame(FrameInput&) noexcept;

        // Completes only received responses. Link closure is observable but does not end payload borrows.
        std::size_t consumeResponses(std::size_t limit = 128) noexcept;
        [[nodiscard]] bool closed() const noexcept;
        [[nodiscard]] std::size_t pendingRequests() const noexcept;

        // The caller must first stop/join the Server and its producers. Delivers Unknown for missing
        // terminal replies, then closes this Link. This is not GPU cancellation or an implicit wait.
        void settleClosed() noexcept;

    private:
        struct Impl;
        explicit RenderRequester(std::unique_ptr<Impl>) noexcept;
        [[nodiscard]] render::RenderResult<render::CommandId>
        findCommand(const render::CommandDescriptor&) const noexcept;
        [[nodiscard]] bool isBuildingInput(const FrameInput&, platform::ipc::RequesterId) const noexcept;
        [[nodiscard]] render::RenderResult<render::RequestId> admit(ResponseNode&, platform::ipc::RequesterId) noexcept;
        [[nodiscard]] render::RenderResult<void>
        sendMessage(render::CommandId, render::RequestId, platform::ipc::IpcMessage&) noexcept;
        void revoke(render::RequestId) noexcept;
        void sendFrame(FrameInput&, ResponseNode&) noexcept;

        template <class Operation, render::RenderCommand C>
        static void finish(ResponseNode& node, platform::ipc::IpcMessage&& message, render::RenderError error) noexcept
        {
            auto& operation = static_cast<Operation&>(node);
            auto response = [&]() noexcept -> typename C::Response
            {
                if (error.type == 0 && message.typeHash() == cxx::typeToken<typename C::Response>().hash())
                {
                    return std::move(message.get<typename C::Response>());
                }
                typename C::Response failed{};
                failed.error = error.type != 0 ? error : render::RenderError{render::kCommandContract};
                return failed;
            }();
            stdexec::set_value(std::move(operation.receiver), std::move(response));
            // Terminal completion may destroy the operation. No further access to it.
        }

        template <render::RenderCommand C>
        void send(BoundCommand<C> command, typename C::Request& input, ResponseNode& node) noexcept
        {
            auto admitted = admit(node, command.source_);
            if (!admitted)
            {
                node.deliver(node, {}, admitted.error());
                return;
            }
            platform::ipc::IpcMessage message;
            message.emplace<typename C::Request>(std::move(input));
            auto sent = sendMessage(command.command_, *admitted, message);
            if (!sent)
            {
                // Restore the same complete Request object, including non-assignable move-only values.
                std::destroy_at(&input);
                std::construct_at(&input, std::move(message.get<typename C::Request>()));
                revoke(*admitted);
                node.deliver(node, {}, sent.error());
            }
        }

        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::rendering
