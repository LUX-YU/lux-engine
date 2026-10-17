#pragma once

#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include "TypedProtocol.hpp"

#ifdef LUX_RUNTIME_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    void replyPublication(const platform::ipc::IpcMessage& message) noexcept;
}
#endif

namespace lux::render::protocol_detail
{
    // Private Runtime control handlers use their actual request owner; external handlers only get Reply<C>.
    struct ReplyAccess final
    {
        template <RenderCommand C> static Reply<C> make(CommandInvocation& invocation) noexcept
        { return Reply<C>{invocation}; }
        template <RenderCommand C> static CommandInvocation& invocation(Reply<C>& reply) noexcept
        { return *reply.invocation_; }
    };
}

namespace lux::render
{
    // The sole source-qualified terminal owner. Public handlers only borrow Reply<C>.
    class RenderRequest final
    {
    public:
        RenderRequest(platform::ipc::ReceivedRequest input, platform::ipc::IpcResponder& responder,
            SessionIdentity session, RequestId request, std::size_t capacity,
            const CommandDescriptor* descriptor, const runtime_detail::CommandTransport* transport,
            const platform::ipc::IpcBoundType<runtime_detail::ValueEnvelope<RenderError>>& failure_type,
            const std::shared_ptr<const void>& response_code_pin,
            const std::shared_ptr<const void>& operation_code_pin) noexcept;
        RenderRequest(RenderRequest&&) noexcept = default;
        RenderRequest& operator=(RenderRequest&& other) noexcept
        {
            if (this == &other) return *this;
            // Destroy all old foreign payloads while their old code pin still exists.
            std::destroy_at(this);
            std::construct_at(this, std::move(other));
            return *this;
        }
        RenderRequest(const RenderRequest&) = delete;
        RenderRequest& operator=(const RenderRequest&) = delete;

        [[nodiscard]] bool active() const noexcept { return input_.active(); }
        [[nodiscard]] RequestId id() const noexcept { return request_; }
        [[nodiscard]] const std::shared_ptr<const void>& codePin() const noexcept { return *operation_code_pin_; }
        [[nodiscard]] std::size_t replyCapacity() const noexcept { return reply_capacity_; }
        template <class T>
        [[nodiscard]] const T& get() const noexcept
        {
            return *static_cast<const T*>(transport_->request_value(input_.message()));
        }

        template <class T>
        [[nodiscard]] RenderResult<void> reply(T value) noexcept { return complete(&value); }
        [[nodiscard]] RenderResult<void> reply() noexcept { return reply(Ack{}); }
        [[nodiscard]] RenderResult<void> complete(void* response) noexcept;
        [[nodiscard]] RenderResult<void> fail(RenderError error) noexcept;
        [[nodiscard]] RenderResult<void> cancel() noexcept;
        [[nodiscard]] RenderResult<void> unknown(RenderError error = {}) noexcept;
        [[nodiscard]] RenderResult<std::size_t> prepareResponse(void*, platform::ipc::IpcMessage&) noexcept;
        [[nodiscard]] RenderResult<void> publishPrepared(platform::ipc::IpcMessage&, std::size_t charge) noexcept;
        [[nodiscard]] CommandInvocation
        invocation(void* call_state, const CommandCompletionFns& functions) noexcept;

    private:
        [[nodiscard]] RenderResult<void> publishError(ECommandOutcome, RenderError) noexcept;
        [[nodiscard]] RenderResult<void> publish(platform::ipc::IpcMessage&, ECommandOutcome) noexcept;
        [[nodiscard]] runtime_detail::BudgetLease splitBudget(std::size_t) noexcept;
        // Borrowed from stable Command metadata (or the bootstrap Impl). All source requests end
        // before that owner; each packaged reply takes its own pin for late foreign destruction.
        const std::shared_ptr<const void>* code_pin_;
        const std::shared_ptr<const void>* operation_code_pin_;
        platform::ipc::ReceivedRequest input_;
        std::unique_ptr<platform::ipc::IpcMessage> unreplied_;
        platform::ipc::IpcResponder* responder_;
        SessionIdentity session_;
        RequestId request_;
        std::size_t reply_capacity_;
        const CommandDescriptor* descriptor_;
        const runtime_detail::CommandTransport* transport_;
        // Borrowed from the same Impl as responder_; all requests are settled before Impl destruction.
        const platform::ipc::IpcBoundType<runtime_detail::ValueEnvelope<RenderError>>* failure_type_;
    };
}
