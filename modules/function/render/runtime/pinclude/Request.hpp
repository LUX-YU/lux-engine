#pragma once

#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include "TypedProtocol.hpp"

namespace lux::render
{
    namespace runtime_detail { struct GpuAdmission; }

    // The sole source-qualified terminal owner. Public handlers only borrow CommandContext<C>.
    class RenderRequest final
    {
    public:
        RenderRequest(platform::ipc::ReceivedRequest input, platform::ipc::IpcResponder& responder,
            SessionIdentity session, RequestId request, std::size_t capacity,
            const CommandDescriptor* descriptor, const runtime_detail::CommandTransport* transport,
            const platform::ipc::IpcBoundType<runtime_detail::ValueEnvelope<RenderError>>& failure_type,
            std::shared_ptr<const void> code_pin = {}) noexcept;
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
        [[nodiscard]] const std::shared_ptr<const void>& codePin() const noexcept { return code_pin_; }
        [[nodiscard]] std::size_t replyCapacity() const noexcept { return reply_capacity_; }
        [[nodiscard]] bool gpuReady() const noexcept
        {
            return descriptor_ && descriptor_->completion == ECompletionContract::RESOURCE_GPU_READY;
        }

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
        [[nodiscard]] RenderResult<void> unknown() noexcept;
        [[nodiscard]] RenderResult<std::size_t> prepareResponse(void*, platform::ipc::IpcMessage&) noexcept;
        [[nodiscard]] RenderResult<void> publishPrepared(platform::ipc::IpcMessage&, std::size_t charge) noexcept;
        [[nodiscard]] CommandInvocation
        invocation(runtime_detail::GpuAdmission& admission, void* extension = nullptr) noexcept;

    private:
        [[nodiscard]] RenderResult<void> publishError(ECommandOutcome, RenderError) noexcept;
        [[nodiscard]] RenderResult<void> publish(platform::ipc::IpcMessage&, ECommandOutcome) noexcept;
        [[nodiscard]] runtime_detail::BudgetLease splitBudget(std::size_t) noexcept;
        // The code pin outlives all payload destruction performed by this request owner.
        std::shared_ptr<const void> code_pin_;
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
