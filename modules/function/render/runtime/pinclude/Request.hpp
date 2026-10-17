#pragma once

#include <optional>
#include <lux/engine/render/runtime/Error.hpp>
#include <lux/engine/render/protocol/CommandHandler.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>

#ifdef LUX_RUNTIME_TEST_SEAM
namespace lux::render::runtime_detail::test
{
    void replyPublication(ReplyHeader header, std::string_view command) noexcept;
}
#endif

namespace lux::render::protocol_detail
{
    struct ReplyAccess final
    {
        template <RenderCommand C>
        static Reply<C> make(CommandInvocation& invocation) noexcept { return Reply<C>{invocation}; }
    };
}

namespace lux::render
{
    // Sole source-qualified owner. ResultReady keeps this same source and the actual response until delivery.
    class RenderRequest final
    {
    public:
        RenderRequest(platform::ipc::ReceivedRequest input, platform::ipc::IpcResponder& responder,
            RequestId request, const CommandHandler* binding) noexcept;
        RenderRequest(RenderRequest&&) noexcept = default;
        RenderRequest& operator=(RenderRequest&&) noexcept = default;
        RenderRequest(const RenderRequest&) = delete;
        RenderRequest& operator=(const RenderRequest&) = delete;
        [[nodiscard]] bool active() const noexcept { return input_.active() && !unreplied_; }
        [[nodiscard]] bool awaitingDelivery() const noexcept { return input_.active() && unreplied_.has_value(); }
        [[nodiscard]] RequestId id() const noexcept { return request_; }

        template <class T>
        [[nodiscard]] const T& get() const noexcept { return input_.message().get<T>(); }

        template <class T>
        [[nodiscard]] RenderResult<void> reply(T value) noexcept { return complete(&value); }

        [[nodiscard]] RenderResult<void> reply() noexcept { return reply(Ack{}); }
        [[nodiscard]] RenderResult<void> complete(void* response) noexcept;
        [[nodiscard]] RenderResult<void> fail(RenderError error) noexcept;
        [[nodiscard]] RenderResult<void> cancel() noexcept;
        [[nodiscard]] RenderResult<void> unknown(RenderError error = {}) noexcept;
        [[nodiscard]] RenderResult<void> prepareResponse(void*, platform::ipc::IpcMessage&) noexcept;
        [[nodiscard]] RenderResult<void> publishPrepared(platform::ipc::IpcMessage&) noexcept;
        void retryDelivery() noexcept;
        [[nodiscard]] CommandInvocation invocation(void* call_state, const CommandCompletionFns& functions) noexcept;

    private:
        [[nodiscard]] RenderResult<void> publishError(ECommandOutcome, RenderError) noexcept;
        [[nodiscard]] RenderResult<void> publish(platform::ipc::IpcMessage&, ECommandOutcome) noexcept;
        platform::ipc::ReceivedRequest input_;
        std::optional<platform::ipc::IpcMessage> unreplied_;
        platform::ipc::IpcResponder* responder_;
        RequestId request_;
        const CommandHandler* binding_; // stable cold metadata; all source requests end before this owner
    };
}
