#pragma once

#include <lux/engine/core/execution/CompletionCode.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/visibility.h>
#include <exec/completion_behavior.hpp>

#include <lux/engine/render/frame/FrameData.hpp>
#include <array>
#include <memory>
#include <tuple>

namespace lux::render
{
    // Single caller-thread protocol endpoint. Start and dispatch on that same thread, without reentrant
    // dispatch. The host supplies its safe completion points and owns Server/thread lifetime.
    class LUX_RENDER_PUBLIC RenderClient final
    {
        struct ResponseNode
        {
            using Deliver =
                void (*)(ResponseNode&, lux::platform::ipc::IpcMessage&&, RenderError) noexcept;
            Deliver deliver;
            cxx::basic_type_info response_type;
            std::shared_ptr<const void> code;
        };

        using CommonCommands = std::tuple<
            QueryRenderJobInputLayout,
            UpdateFrameData,
            QueryResource,
            GetOrCreateResource,
            CreateScene,
            RemoveScene,
            CreateView,
            RemoveView,
            CreateTarget,
            ResizeTarget,
            RemoveTarget,
            SetTargetActive,
            CreateEffect,
            SetToneMappingExposure,
            RemoveEffect,
            CreateRenderJob,
            StartRenderJob,
            StopRenderJob,
            RemoveRenderJob,
            RebindRenderJobEffect,
            SetRenderJobCadence,
            SetRenderJobDependencies,
            RequestRenderJob,
            UpdateJobInputs,
            InvalidateHistory,
            QueryExecutionProgress,
            QueryRenderStats,
            CreateDeviceSession,
            RemoveDeviceSession,
            QueryRenderDevice,
            Stop,
            ListEffectCapabilities,
            ListCommands
        >;

        template <class C> static consteval std::size_t commonIndex() noexcept
        {
            return []<class... Commands>(std::type_identity<std::tuple<Commands...>>) consteval
            {
                constexpr std::array types{cxx::make_basic_type_info<Commands>()...};
                for (std::size_t i = 0; i < types.size(); ++i)
                {
                    if (types[i] == cxx::make_basic_type_info<C>())
                    {
                        return i;
                    }
                }
                return types.size();
            }(std::type_identity<CommonCommands>{});
        }

        template <RenderCommand C> class RequestSender final
        {
            static constexpr auto response_type_info = cxx::make_basic_type_info<typename C::Response>();

            struct Environment final
            {
                [[nodiscard]] auto query(exec::get_completion_behavior_t<stdexec::set_value_t>) const noexcept
                {
                    return exec::completion_behavior::asynchronous_affine;
                }
            };

        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(typename C::Response)>;

            RequestSender(RenderClient& client, CommandId command, typename C::Request& input) noexcept
                : client_(client), command_(command), input_(input)
            {
            }

            [[nodiscard]] Environment get_env() const noexcept { return {}; }

            template <class Receiver> struct Operation final : ResponseNode
            {
                using operation_state_concept = stdexec::operation_state_t;
                RenderClient& client;
                CommandId command;
                typename C::Request& input;
                Receiver receiver;
                FrameData* borrowed_frame = nullptr;

                Operation(
                    RenderClient& client,
                    CommandId command,
                    typename C::Request& input,
                    Receiver receiver
                ) noexcept
                    : ResponseNode{
                          &finish, response_type_info,
                          execution::detail::getCompletionCode(stdexec::get_env(receiver))
                      },
                      client(client), command(command), input(input), receiver(std::move(receiver))
                {
                }

                static void finish(
                    ResponseNode& node, lux::platform::ipc::IpcMessage&& message, RenderError error
                ) noexcept
                {
                    auto& operation = static_cast<Operation&>(node);
                    if (operation.borrowed_frame)
                    {
                        operation.client.releaseFrame(*operation.borrowed_frame);
                        operation.borrowed_frame = nullptr;
                    }
                    if (error.type != 0)
                    {
                        typename C::Response response{};
                        response.error = error;
                        stdexec::set_value(std::move(operation.receiver), std::move(response));
                    }
                    else
                    {
                        auto response = std::move(message.get<typename C::Response>());
                        stdexec::set_value(std::move(operation.receiver), std::move(response));
                    }
                }

                void start() & noexcept
                {
                    if constexpr (requires { C::frameData(input); })
                    {
                        auto* packet = C::frameData(input);
                        if (packet)
                        {
                            if (auto result = client.borrowFrame(*packet); !result)
                            {
                                RenderClient::deliver(*this, {}, result.error());
                                return;
                            }
                        }
                        borrowed_frame = packet;
                    }
                    if (stdexec::get_stop_token(stdexec::get_env(receiver)).stop_requested())
                    {
                        RenderClient::deliver(*this, {}, {kCommandCancelled});
                        return;
                    }
                    lux::platform::ipc::IpcMessage message;
                    message.emplace<typename C::Request>(std::move(input));
                    auto accepted = client.send(command, message, *this);
                    if (!accepted)
                    {
                        // Failed publication acquired no borrow. Restore the caller's move-only request.
                        std::destroy_at(&input);
                        std::construct_at(&input, std::move(message.get<typename C::Request>()));
                        RenderClient::deliver(*this, {}, accepted.error());
                    }
                }
            };

            template <class Receiver> [[nodiscard]] auto connect(Receiver receiver) const noexcept
            {
                return Operation<Receiver>{client_, command_, input_, std::move(receiver)};
            }

        private:
            RenderClient& client_;
            CommandId command_;
            typename C::Request& input_;
        };

    public:
        using CreateResult = RenderResult<std::unique_ptr<RenderClient>>;

        // Cold bootstrap only: Server must already be running. Performs one batch IPC query before
        // publishing the Client, with no business operations or Main callbacks being dispatched.
        // The existing host reply notification may enqueue work; it must not dereference this Client
        // inline. Failure returns no Client; the host still owns service stop/join.
        [[nodiscard]] static CreateResult create(lux::platform::ipc::IpcRequester client) noexcept;

        ~RenderClient() noexcept;
        RenderClient(const RenderClient&) = delete;
        RenderClient& operator=(const RenderClient&) = delete;
        RenderClient(RenderClient&&) = delete;
        RenderClient& operator=(RenderClient&&) = delete;

        // Start and dispatch on the caller thread. A pre-publication rejection may complete inline.
        // Successful publication moves input into IPC; failed publication restores it. Borrowed source
        // bytes and the connected operation remain valid until the real Response.
        // Accepted requests complete even after cancellation; cancellation cannot shorten the Server's borrow.
        template <RenderCommand C> [[nodiscard]] auto request(typename C::Request& input) noexcept
        {
            if constexpr (std::same_as<C, QueryCommandIds>)
            {
                return RequestSender<C>{*this, QueryCommandIds::id, input};
            }
            else
            {
                constexpr auto index = commonIndex<C>();
                static_assert(index < std::tuple_size_v<CommonCommands>,
                    "Resolve professional CommandIds during asynchronous assembly and pass the ID explicitly");
                return RequestSender<C>{*this, commonCommand(index), input};
            }
        }

        // command must be the ID obtained for C from this Server. No repeated name lookup or binding object.
        template <RenderCommand C>
        [[nodiscard]] auto request(CommandId command, typename C::Request& input) noexcept
        {
            return RequestSender<C>{*this, command, input};
        }

        // Own the request object through completion; spans/pointers inside it still borrow their sources.
        template <RenderCommand C> [[nodiscard]] auto request(typename C::Request&& input) noexcept
        {
            return stdexec::just(std::move(input)) |
                stdexec::let_value([this](typename C::Request& owned) noexcept { return request<C>(owned); });
        }

        template <RenderCommand C>
        [[nodiscard]] auto request(CommandId command, typename C::Request&& input) noexcept
        {
            return stdexec::just(std::move(input)) |
                stdexec::let_value([this, command](typename C::Request& owned) noexcept
                {
                    return request<C>(command, owned);
                }
            );
        }

        // Host safe point only; limit is positive. Returns consumed messages, not outstanding requests.
        // A full batch needs another host dispatch. Transport faults reject new requests but retain
        // accepted borrows until real delivery or serverStopped. This never stops or joins Server.
        [[nodiscard]] RenderResult<std::size_t> dispatchResponses(std::size_t limit) noexcept;

        // Call only after Server has returned, settled its work, and its thread has joined. Delivers
        // published replies first, then unknown outcomes. No caller may destroy this Client in delivery.
        void serverStopped(RenderError error) noexcept;

    private:
        struct Impl;
        explicit RenderClient(std::unique_ptr<Impl>) noexcept;
        [[nodiscard]] CommandId commonCommand(std::size_t index) const noexcept;
        [[nodiscard]] RenderResult<void> send(CommandId, lux::platform::ipc::IpcMessage&, ResponseNode&) noexcept;
        [[nodiscard]] RenderResult<void> borrowFrame(FrameData&) noexcept;
        void releaseFrame(FrameData&) noexcept;
        static void deliver(ResponseNode&, lux::platform::ipc::IpcMessage&&, RenderError) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::render
