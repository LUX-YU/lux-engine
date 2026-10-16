#pragma once

#include <lux/engine/render/runtime/CommandValue.hpp>
#include <lux/engine/render/data/Payload.hpp>

namespace lux::render
{
    class LUX_RENDER_CLIENT_PUBLIC RenderCompletion;
    template <RenderCommand C> class TypedRenderCompletion;

    inline constexpr auto kRuntimeState = error::errorId("lux.render.runtime.state");
    inline constexpr auto kRuntimeCapacity = error::errorId("lux.render.runtime.capacity");
    inline constexpr auto kRuntimeIdentity = error::errorId("lux.render.runtime.identity");
    inline constexpr auto kRuntimeUnsupported = error::errorId("lux.render.runtime.unsupported");
    [[nodiscard]] LUX_RENDER_CLIENT_PUBLIC std::span<const error::ErrorDescriptor>
    renderRuntimeErrorDescriptors() noexcept;

    template <RenderCommand C>
    class BoundCommand final
    {
    public:
        [[nodiscard]] SessionIdentity session() const noexcept { return session_; }
        [[nodiscard]] CommandId command() const noexcept { return command_; }

    private:
        friend class RenderClient;
        friend class RenderCompletion;
        using Input = runtime_detail::ValueEnvelope<typename C::Request>;
        using Output = runtime_detail::ValueEnvelope<typename C::Response>;
        BoundCommand(SessionIdentity session, CommandId command, platform::ipc::IpcBoundType<Input> request,
            platform::ipc::IpcBoundType<Output> response) noexcept
            : session_(session), command_(command), request_(std::move(request)), response_(std::move(response)) {}
        SessionIdentity session_{};
        CommandId command_;
        platform::ipc::IpcBoundType<Input> request_;
        platform::ipc::IpcBoundType<Output> response_;
    };

    enum class ERenderReceive { RECEIVED, EMPTY, CLOSED };

    class LUX_RENDER_CLIENT_PUBLIC RenderCompletion final
    {
    public:
        ReplyHeader header;
        SessionIdentity session{};
        RenderCompletion() noexcept = default;
        ~RenderCompletion() noexcept = default;
        RenderCompletion(RenderCompletion&& other) noexcept;
        RenderCompletion& operator=(RenderCompletion&& other) noexcept;
        RenderCompletion(const RenderCompletion&) = delete;
        RenderCompletion& operator=(const RenderCompletion&) = delete;
        [[nodiscard]] bool active() const noexcept { return !header.request.isNull(); }
        [[nodiscard]] CommandId command() const noexcept { return command_; }

        template <RenderCommand C>
        [[nodiscard]] RenderResult<TypedRenderCompletion<C>> take(const BoundCommand<C>& binding) && noexcept;

    private:
        friend class RenderClient;
        template <RenderCommand C> friend class TypedRenderCompletion;
        platform::ipc::IpcReply backing_;
        CommandId command_;
    };

    template <RenderCommand C>
    class TypedRenderCompletion final
    {
    public:
        TypedRenderCompletion(TypedRenderCompletion&&) noexcept = default;
        TypedRenderCompletion& operator=(TypedRenderCompletion&&) noexcept = default;
        TypedRenderCompletion(const TypedRenderCompletion&) = delete;
        TypedRenderCompletion& operator=(const TypedRenderCompletion&) = delete;
        [[nodiscard]] bool active() const noexcept { return completion_.active(); }
        [[nodiscard]] const ReplyHeader& header() const noexcept { return completion_.header; }
        [[nodiscard]] SessionIdentity session() const noexcept { return completion_.session; }
        // Borrows end with this completion. Explicitly recovering/copying a business value transfers it
        // to application ownership; the transport budget is not a whole-process asset memory limit.
        // A failed/cancelled completion still owns its returned Request. Its error is in header().
        [[nodiscard]] const typename C::Response* tryResponse() const & noexcept
        {
            if (header().outcome != ECommandOutcome::SUCCESS) return nullptr;
            return &completion_.backing_.message.template get<runtime_detail::ValueEnvelope<typename C::Response>>().value;
        }

        // Precondition: SUCCESS. Never interpret an error object as a Response, including release builds.
        [[nodiscard]] const typename C::Response& response() const & noexcept
        {
            const auto* value = tryResponse();
            if (!value) std::terminate();
            return *value;
        }

        [[nodiscard]] typename C::Request& request() & noexcept
        {
            return completion_.backing_.original_request.template get<runtime_detail::ValueEnvelope<typename C::Request>>().value;
        }

    private:
        friend class RenderCompletion;
        explicit TypedRenderCompletion(RenderCompletion completion) noexcept : completion_(std::move(completion)) {}
        RenderCompletion completion_;
    };

    template <RenderCommand C>
    RenderResult<TypedRenderCompletion<C>> RenderCompletion::take(const BoundCommand<C>& binding) && noexcept
    {
        const bool is_wrong_identity = !active() || session != binding.session_ || command_ != binding.command_;
        if (is_wrong_identity) return cxx::unexpected(RenderError{kRuntimeIdentity});
        // Closure may report an unknown outcome without returning the Request. Do not invent recovery.
        if (backing_.original_request.empty())
            return cxx::unexpected(header.error.type ? header.error : RenderError{kRuntimeState});
        const bool is_wrong_request = !backing_.original_request.holds(binding.request_.operations());
        const bool is_wrong_response = header.outcome == ECommandOutcome::SUCCESS &&
            !backing_.message.holds(binding.response_.operations());
        if (is_wrong_request || is_wrong_response) return cxx::unexpected(RenderError{kProtocolContract});
        return TypedRenderCompletion<C>{std::move(*this)};
    }

    struct RenderClientConfig final
    {
        std::size_t retained_byte_limit = 64 * 1024 * 1024;
        std::size_t ring_capacity = 64;
    };

    // Main and Upload are long-lived SPSC owners. The host supplies their threads.
    class LUX_RENDER_CLIENT_PUBLIC RenderClient final
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<RenderClient>>
        create(std::string_view channel_name, RenderClientConfig config = {}) noexcept;
        ~RenderClient() noexcept;
        // create() allocates in this DLL; default_delete must release in the same DLL.
        static void operator delete(void* value) noexcept;
        RenderClient(const RenderClient&) = delete;
        RenderClient& operator=(const RenderClient&) = delete;
        [[nodiscard]] RenderResult<RequestId> hello() noexcept;
        [[nodiscard]] SessionIdentity session() const noexcept;

        template <RenderCommand C>
        [[nodiscard]] RenderResult<BoundCommand<C>> bind() const noexcept
        {
            auto binding = bindCommand(commandDescriptor<C>());
            if (!binding) return cxx::unexpected(binding.error());
            auto request = platform::ipc::IpcMessage::bindType<runtime_detail::ValueEnvelope<typename C::Request>>(
                *binding->request, binding->code_pin);
            auto response = platform::ipc::IpcMessage::bindType<runtime_detail::ValueEnvelope<typename C::Response>>(
                *binding->response, binding->code_pin);
            if (!request || !response) return cxx::unexpected(RenderError{kProtocolContract});
            return BoundCommand<C>{binding->session, binding->command, std::move(*request), std::move(*response)};
        }

        template <RenderCommand C>
        [[nodiscard]] RenderResult<RequestId> submit(const BoundCommand<C>& binding,
            typename C::Request& request, std::size_t reply_capacity = 128) noexcept
        {
            using Input = runtime_detail::ValueEnvelope<typename C::Request>;
            using Output = runtime_detail::ValueEnvelope<typename C::Response>;
            auto bytes = CommandValueTraits<typename C::Request>::retainedBytes(request);
            if (!bytes) return cxx::unexpected(bytes.error());
            constexpr auto response_overhead = sizeof(Output) - sizeof(typename C::Response);
            constexpr auto error_overhead = sizeof(runtime_detail::ValueEnvelope<RenderError>) - sizeof(RenderError);
            constexpr auto overhead = sizeof(Input) - sizeof(typename C::Request) +
                (response_overhead > error_overhead ? response_overhead : error_overhead);
            auto reservation = reserve(binding.session_, binding.command_, &binding.response_.operations(),
                *bytes, reply_capacity, overhead);
            if (!reservation) return cxx::unexpected(reservation.error());
            platform::ipc::IpcMessage message;
            message.emplace(binding.request_, std::move(reservation->second), std::move(request));
            auto published = publish(reservation->first, message, binding.session_, binding.command_, reply_capacity);
            if (!published)
            {
                // No noexcept assignment requirement: restore the same Request object in place.
                std::destroy_at(std::addressof(request));
                std::construct_at(std::addressof(request), std::move(message.template get<Input>().value));
                return cxx::unexpected(published.error());
            }
            return reservation->first;
        }

        // output must be inactive. At most eight abandoned records are reclaimed per call.
        [[nodiscard]] RenderResult<ERenderReceive> tryReceive(RenderCompletion& output) noexcept;
        [[nodiscard]] RenderResult<void> abandon(RequestId request) noexcept;
        [[nodiscard]] std::size_t retainedBytes() const noexcept;

    private:
        struct Binding final
        {
            SessionIdentity session;
            CommandId command;
            const platform::ipc::IpcTypeOps* request;
            const platform::ipc::IpcTypeOps* response;
            std::shared_ptr<const void> code_pin;
        };

        [[nodiscard]] RenderResult<Binding> bindCommand(const CommandDescriptor& descriptor) const noexcept;
        [[nodiscard]] RenderResult<std::pair<RequestId, runtime_detail::BudgetLease>> reserve(
            SessionIdentity session, CommandId command, const platform::ipc::IpcTypeOps* response,
            std::size_t request_bytes, std::size_t reply_capacity, std::size_t envelope_bytes) noexcept;
        [[nodiscard]] RenderResult<void> publish(RequestId request, platform::ipc::IpcMessage& message,
            SessionIdentity session, CommandId command, std::size_t reply_capacity) noexcept;
        struct Impl;
        explicit RenderClient(std::unique_ptr<Impl> state) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}
